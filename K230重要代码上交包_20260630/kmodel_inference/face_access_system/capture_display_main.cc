#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <termios.h>
#include <unistd.h>

#include <opencv2/imgproc.hpp>

extern "C"
{
#include <lwp_shm.h>
}

#include "face_access_common.h"
#include "scoped_timing.hpp"
#include "vi_vo.h"

namespace
{
std::atomic<bool> g_stop(false);
int g_stdin_flags = -1;
struct termios g_saved_terminal;
bool g_terminal_saved = false;

uint64_t monotonic_ms()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

void set_terminal_raw(bool raw)
{
    if (!g_terminal_saved)
    {
        if (tcgetattr(STDIN_FILENO, &g_saved_terminal) == 0)
            g_terminal_saved = true;
    }
    if (!g_terminal_saved)
        return;

    struct termios current = g_saved_terminal;
    if (raw)
        current.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &current);

    if (g_stdin_flags < 0)
        g_stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (g_stdin_flags >= 0)
        fcntl(STDIN_FILENO, F_SETFL, raw ? (g_stdin_flags | O_NONBLOCK) : g_stdin_flags);
}

void publish_command(face_access_shared_t *shared, uint32_t type, const std::string &name)
{
    shared->command.type = type;
    std::memset(shared->command.name, 0, sizeof(shared->command.name));
    if (!name.empty())
        std::strncpy(shared->command.name, name.c_str(), sizeof(shared->command.name) - 1);
    face_atomic_store_u32(&shared->command_sequence,
                          face_atomic_load_u32(&shared->command_sequence) + 1);
}

void handle_keyboard(face_access_shared_t *shared)
{
    char key = 0;
    if (read(STDIN_FILENO, &key, 1) <= 0)
        return;

    if (key == 'i')
    {
        set_terminal_raw(false);
        std::string name;
        std::cout << "\nName to register: " << std::flush;
        std::getline(std::cin, name);
        if (name.empty())
            std::getline(std::cin, name);
        if (!name.empty())
            publish_command(shared, FACE_COMMAND_REGISTER, name);
        set_terminal_raw(true);
    }
    else if (key == 'r')
    {
        publish_command(shared, FACE_COMMAND_RESET_DB, "");
    }
    else if (key == 27 || key == 'q')
    {
        publish_command(shared, FACE_COMMAND_STOP, "");
        face_atomic_store_u32(&shared->stop_requested, 1);
        g_stop = true;
    }
}

void handle_command_file(face_access_shared_t *shared, const char *path)
{
    std::ifstream input(path);
    if (!input)
        return;
    std::string command;
    std::getline(input, command);
    input.close();
    if (command.rfind("REGISTER:", 0) == 0 && command.size() > 9)
        publish_command(shared, FACE_COMMAND_REGISTER, command.substr(9));
    else if (command == "RESET")
        publish_command(shared, FACE_COMMAND_RESET_DB, "");
    else if (command == "STOP")
    {
        publish_command(shared, FACE_COMMAND_STOP, "");
        face_atomic_store_u32(&shared->stop_requested, 1);
        g_stop = true;
    }
    std::remove(path);
}

bool read_latest_result(face_access_shared_t *shared, face_access_result_t &result,
                        uint32_t &last_sequence)
{
    const uint32_t before = face_atomic_load_u32(&shared->result_sequence);
    if (before == 0 || before == last_sequence)
        return false;
    std::memcpy(&result, &shared->result, sizeof(result));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (before != face_atomic_load_u32(&shared->result_sequence))
        return false;
    last_sequence = before;
    return true;
}

void draw_result(cv::Mat &osd, const face_access_result_t &result)
{
    if (result.status == FACE_STATUS_NONE)
        return;

    const int x = result.x * osd.cols / SENSOR_WIDTH;
    const int y = result.y * osd.rows / SENSOR_HEIGHT;
    const int w = result.width * osd.cols / SENSOR_WIDTH;
    const int h = result.height * osd.rows / SENSOR_HEIGHT;
    const cv::Scalar color = result.status == FACE_STATUS_RECOGNIZED
                                 ? cv::Scalar(0, 255, 0, 255)
                                 : cv::Scalar(0, 0, 255, 255);
    char label[96];
    if (result.status == FACE_STATUS_RECOGNIZED)
        std::snprintf(label, sizeof(label), "%s %.1f", result.name, result.similarity);
    else
        std::snprintf(label, sizeof(label), "unknown %.1f", result.similarity);

    cv::rectangle(osd, cv::Rect(x, y, w, h), color, 5);
    cv::putText(osd, label, cv::Point(x, std::max(40, y - 10)),
                cv::FONT_HERSHEY_SIMPLEX, 1.5, color, 3);
}
} // namespace

int main(int argc, char **argv)
{
    const int debug_mode = argc > 1 ? std::atoi(argv[1]) : 0;
    const uint32_t frame_size = SENSOR_CHANNEL * SENSOR_HEIGHT * SENSOR_WIDTH;

    const int shmid = lwp_shmget(FACE_ACCESS_SHM_KEY, sizeof(face_access_shared_t), 1);
    if (shmid < 0)
    {
        std::cerr << "cannot create face access shared memory" << std::endl;
        return 1;
    }
    auto *shared = static_cast<face_access_shared_t *>(lwp_shmat(shmid, nullptr));
    if (shared == nullptr)
    {
        std::cerr << "cannot attach face access shared memory" << std::endl;
        lwp_shmrm(shmid);
        return 1;
    }
    std::memset(shared, 0, sizeof(*shared));
    shared->magic = FACE_ACCESS_MAGIC;
    shared->version = FACE_ACCESS_VERSION;

    vivcap_start();
    k_u64 frame_paddr[FACE_ACCESS_FRAME_SLOTS] = {0};
    void *frame_vaddr[FACE_ACCESS_FRAME_SLOTS] = {nullptr};
    for (uint32_t i = 0; i < FACE_ACCESS_FRAME_SLOTS; ++i)
    {
        const int ret = kd_mpi_sys_mmz_alloc(&frame_paddr[i], &frame_vaddr[i],
                                              "face_frame", "anonymous", frame_size);
        if (ret != K_SUCCESS)
        {
            std::cerr << "MMZ allocation failed for frame slot " << i << ": " << ret << std::endl;
            face_atomic_store_u32(&shared->stop_requested, 1);
            for (uint32_t j = 0; j < i; ++j)
                kd_mpi_sys_mmz_free(frame_paddr[j], nullptr);
            vivcap_stop();
            lwp_shmdt(shared);
            lwp_shmrm(shmid);
            return 1;
        }
        shared->frames[i].physical_address = frame_paddr[i];
        shared->frames[i].data_size = frame_size;
        shared->frames[i].width = SENSOR_WIDTH;
        shared->frames[i].height = SENSOR_HEIGHT;
        shared->frames[i].channels = SENSOR_CHANNEL;
        face_atomic_store_u32(&shared->frames[i].state, FACE_SLOT_FREE);
    }

    k_video_frame_info vf_info;
    void *pic_vaddr = nullptr;
    std::memset(&vf_info, 0, sizeof(vf_info));
    vf_info.v_frame.width = osd_width;
    vf_info.v_frame.height = osd_height;
    vf_info.v_frame.stride[0] = osd_width;
    vf_info.v_frame.pixel_format = PIXEL_FORMAT_ARGB_8888;
    block = vo_insert_frame(&vf_info, &pic_vaddr);

    set_terminal_raw(true);
    std::cout << "capture_display ready: i=register, r=reset database, q/ESC=quit" << std::endl;
    face_atomic_store_u32(&shared->ready, 1);

    uint32_t frame_sequence = 0;
    uint32_t result_sequence = 0;
    uint32_t command_poll_divider = 0;
    face_access_result_t latest_result{};

    while (!g_stop && !face_atomic_load_u32(&shared->stop_requested))
    {
        ScopedTiming total("capture/display total", debug_mode);
        handle_keyboard(shared);
        if (++command_poll_divider >= 15)
        {
            handle_command_file(shared, "/sharefs/face_access/command.txt");
            command_poll_divider = 0;
        }

        std::memset(&dump_info, 0, sizeof(k_video_frame_info));
        int ret = kd_mpi_vicap_dump_frame(vicap_dev, VICAP_CHN_ID_1,
                                           VICAP_DUMP_YUV, &dump_info, 1000);
        if (ret != K_SUCCESS)
            continue;

        int selected = -1;
        for (uint32_t i = 0; i < FACE_ACCESS_FRAME_SLOTS; ++i)
        {
            if (face_atomic_cas_u32(&shared->frames[i].state,
                                    FACE_SLOT_FREE, FACE_SLOT_WRITING))
            {
                selected = static_cast<int>(i);
                break;
            }
        }

        if (selected >= 0)
        {
            void *camera = kd_mpi_sys_mmap(dump_info.v_frame.phys_addr[0], frame_size);
            if (camera != nullptr)
            {
                std::memcpy(frame_vaddr[selected], camera, frame_size);
                kd_mpi_sys_munmap(camera, frame_size);
                auto &slot = shared->frames[selected];
                slot.sequence = ++frame_sequence;
                slot.captured_at_ms = monotonic_ms();
                face_atomic_store_u32(&slot.state, FACE_SLOT_READY);
            }
            else
            {
                face_atomic_store_u32(&shared->frames[selected].state, FACE_SLOT_FREE);
            }
        }

        read_latest_result(shared, latest_result, result_sequence);
        cv::Mat osd(osd_height, osd_width, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        cv::rotate(osd, osd, cv::ROTATE_90_COUNTERCLOCKWISE);
        draw_result(osd, latest_result);
        cv::rotate(osd, osd, cv::ROTATE_90_CLOCKWISE);
        std::memcpy(pic_vaddr, osd.data, osd_width * osd_height * 4);
        kd_mpi_vo_chn_insert_frame(osd_id + 3, &vf_info);

        kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
        __atomic_add_fetch(&shared->capture_heartbeat, 1, __ATOMIC_RELAXED);
    }

    face_atomic_store_u32(&shared->stop_requested, 1);
    set_terminal_raw(false);
    vo_osd_release_block();
    vivcap_stop();
    usleep(200000);
    for (uint32_t i = 0; i < FACE_ACCESS_FRAME_SLOTS; ++i)
        kd_mpi_sys_mmz_free(frame_paddr[i], nullptr);
    lwp_shmdt(shared);
    lwp_shmrm(shmid);
    return 0;
}
