#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

extern "C"
{
#include <lwp_shm.h>
}

#include "face_access_common.h"

#define GPIO_DM_OUTPUT _IOW('G', 0, int)
#define GPIO_WRITE_LOW _IOW('G', 4, int)
#define GPIO_WRITE_HIGH _IOW('G', 5, int)

namespace
{
struct pin_mode_t
{
    unsigned short pin;
    unsigned short mode;
};

uint64_t monotonic_ms()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

class AccessHardware
{
public:
    bool open_device(bool enabled, int relay_pin, int buzzer_pin)
    {
        enabled_ = enabled;
        relay_.pin = static_cast<unsigned short>(relay_pin);
        buzzer_.pin = static_cast<unsigned short>(buzzer_pin);
        if (!enabled_)
        {
            std::cout << "GPIO disabled (dry-run mode)" << std::endl;
            return true;
        }
        fd_ = open("/dev/gpio", O_RDWR);
        if (fd_ < 0)
        {
            std::perror("open /dev/gpio");
            return false;
        }
        if (ioctl(fd_, GPIO_DM_OUTPUT, &relay_) != 0 ||
            ioctl(fd_, GPIO_DM_OUTPUT, &buzzer_) != 0)
        {
            std::perror("configure GPIO output");
            close(fd_);
            fd_ = -1;
            return false;
        }
        relay_off();
        buzzer_off();
        return true;
    }

    ~AccessHardware()
    {
        relay_off();
        buzzer_off();
        if (fd_ >= 0)
            close(fd_);
    }

    void relay_on()
    {
        std::cout << "[DOOR] relay ON" << std::endl;
        write_pin(relay_, true);
    }

    void relay_off()
    {
        write_pin(relay_, false);
    }

    void success_beep()
    {
        write_pin(buzzer_, true);
        usleep(100000);
        write_pin(buzzer_, false);
    }

    void alarm_beep()
    {
        std::cout << "[ALARM] unknown face" << std::endl;
        for (int i = 0; i < 3; ++i)
        {
            write_pin(buzzer_, true);
            usleep(150000);
            write_pin(buzzer_, false);
            usleep(100000);
        }
    }

private:
    void buzzer_off() { write_pin(buzzer_, false); }

    void write_pin(pin_mode_t &pin, bool high)
    {
        if (enabled_ && fd_ >= 0)
            ioctl(fd_, high ? GPIO_WRITE_HIGH : GPIO_WRITE_LOW, &pin);
    }

    int fd_ = -1;
    bool enabled_ = false;
    pin_mode_t relay_{};
    pin_mode_t buzzer_{};
};

face_access_shared_t *wait_for_shared_state(int &shmid)
{
    while (true)
    {
        shmid = lwp_shmget(FACE_ACCESS_SHM_KEY, sizeof(face_access_shared_t), 0);
        if (shmid >= 0)
        {
            auto *shared = static_cast<face_access_shared_t *>(lwp_shmat(shmid, nullptr));
            if (shared != nullptr && shared->magic == FACE_ACCESS_MAGIC)
                return shared;
            if (shared != nullptr)
                lwp_shmdt(shared);
        }
        usleep(100000);
    }
}

void append_log(const std::string &path, const face_access_result_t &event,
                const char *action)
{
    bool write_header = false;
    std::ifstream existing(path);
    if (!existing.good() || existing.peek() == std::ifstream::traits_type::eof())
        write_header = true;
    existing.close();

    std::ofstream log(path, std::ios::app);
    if (!log)
    {
        std::cerr << "cannot open access log: " << path << std::endl;
        return;
    }
    if (write_header)
        log << "monotonic_ms,frame_id,person_id,name,similarity,status,latency_ms,action\n";
    log << event.completed_at_ms << ',' << event.frame_sequence << ',' << event.person_id
        << ',' << event.name << ',' << event.similarity << ',' << event.status << ','
        << event.latency_ms << ',' << action << '\n';
    log.flush();
}
} // namespace

int main(int argc, char **argv)
{
    std::string log_path = "/sharefs/face_access/access.csv";
    bool gpio_enabled = false;
    int relay_pin = 27;
    int buzzer_pin = 46;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--gpio") == 0)
            gpio_enabled = true;
        else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            log_path = argv[++i];
        else if (std::strcmp(argv[i], "--relay-pin") == 0 && i + 1 < argc)
            relay_pin = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--buzzer-pin") == 0 && i + 1 < argc)
            buzzer_pin = std::atoi(argv[++i]);
    }

    AccessHardware hardware;
    if (!hardware.open_device(gpio_enabled, relay_pin, buzzer_pin))
        return 1;

    int shmid = -1;
    face_access_shared_t *shared = wait_for_shared_state(shmid);
    std::cout << "access_control ready, relay=" << relay_pin
              << ", buzzer=" << buzzer_pin << std::endl;

    uint32_t last_result_sequence = 0;
    int last_person_id = -1;
    int recognized_count = 0;
    int unknown_count = 0;
    uint64_t last_open_ms = 0;

    constexpr int required_recognized_frames = 3;
    constexpr int required_unknown_frames = 3;
    constexpr uint64_t cooldown_ms = 3000;
    constexpr useconds_t relay_open_us = 1000000;

    while (!face_atomic_load_u32(&shared->stop_requested))
    {
        const uint32_t sequence = face_atomic_load_u32(&shared->result_sequence);
        if (sequence == 0 || sequence == last_result_sequence)
        {
            usleep(5000);
            continue;
        }

        face_access_result_t event{};
        std::memcpy(&event, &shared->result, sizeof(event));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (sequence != face_atomic_load_u32(&shared->result_sequence))
            continue;
        last_result_sequence = sequence;

        if (event.status == FACE_STATUS_RECOGNIZED)
        {
            unknown_count = 0;
            if (event.person_id == last_person_id)
                ++recognized_count;
            else
            {
                last_person_id = event.person_id;
                recognized_count = 1;
            }

            const uint64_t now = monotonic_ms();
            if (recognized_count >= required_recognized_frames &&
                now - last_open_ms >= cooldown_ms)
            {
                append_log(log_path, event, "OPEN");
                hardware.success_beep();
                hardware.relay_on();
                usleep(relay_open_us);
                hardware.relay_off();
                last_open_ms = now;
                recognized_count = 0;
            }
        }
        else if (event.status == FACE_STATUS_UNKNOWN)
        {
            recognized_count = 0;
            last_person_id = -1;
            if (++unknown_count >= required_unknown_frames)
            {
                append_log(log_path, event, "ALARM");
                hardware.alarm_beep();
                unknown_count = 0;
            }
        }
        else if (event.status == FACE_STATUS_REGISTERED)
        {
            append_log(log_path, event, "REGISTER");
            std::cout << "registered: " << event.name << std::endl;
        }
        else if (event.status == FACE_STATUS_DB_RESET)
        {
            append_log(log_path, event, "RESET_DB");
        }
        else if (event.status == FACE_STATUS_NONE)
        {
            recognized_count = 0;
            unknown_count = 0;
            last_person_id = -1;
        }
        __atomic_add_fetch(&shared->control_heartbeat, 1, __ATOMIC_RELAXED);
    }

    lwp_shmdt(shared);
    return 0;
}
