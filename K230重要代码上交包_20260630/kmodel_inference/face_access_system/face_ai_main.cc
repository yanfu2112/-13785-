#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C"
{
#include <lwp_shm.h>
}

#include "face_access_common.h"
#include "face_detection.h"
#include "face_recognition.h"
#include "mpi_sys_api.h"

namespace
{
uint64_t monotonic_ms()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

face_access_shared_t *wait_for_shared_state(int &shmid)
{
    while (true)
    {
        shmid = lwp_shmget(FACE_ACCESS_SHM_KEY, sizeof(face_access_shared_t), 0);
        if (shmid >= 0)
        {
            auto *shared = static_cast<face_access_shared_t *>(lwp_shmat(shmid, nullptr));
            if (shared != nullptr && shared->magic == FACE_ACCESS_MAGIC &&
                shared->version == FACE_ACCESS_VERSION)
                return shared;
            if (shared != nullptr)
                lwp_shmdt(shared);
        }
        usleep(100000);
    }
}

void publish_result(face_access_shared_t *shared, const face_access_result_t &result)
{
    std::memcpy(&shared->result, &result, sizeof(result));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    face_atomic_store_u32(&shared->result_sequence,
                          face_atomic_load_u32(&shared->result_sequence) + 1);
}

void fill_result(face_access_result_t &out, const face_access_frame_slot_t &slot,
                 const FaceDetectionInfo &face, const FaceRecognitionInfo &recognition,
                 float recognition_threshold)
{
    std::memset(&out, 0, sizeof(out));
    out.frame_sequence = slot.sequence;
    out.person_id = recognition.id;
    out.similarity = recognition.score;
    out.x = static_cast<int32_t>(face.bbox.x);
    out.y = static_cast<int32_t>(face.bbox.y);
    out.width = static_cast<int32_t>(face.bbox.w);
    out.height = static_cast<int32_t>(face.bbox.h);
    out.captured_at_ms = slot.captured_at_ms;
    out.completed_at_ms = monotonic_ms();
    out.latency_ms = static_cast<uint32_t>(out.completed_at_ms - out.captured_at_ms);
    if (recognition.id >= 0 && recognition.score >= recognition_threshold)
    {
        out.status = FACE_STATUS_RECOGNIZED;
        std::strncpy(out.name, recognition.name.c_str(), sizeof(out.name) - 1);
    }
    else
    {
        out.status = FACE_STATUS_UNKNOWN;
        std::strncpy(out.name, "unknown", sizeof(out.name) - 1);
    }
}
} // namespace

void print_usage(const char *program)
{
    std::cout << "Usage: " << program
              << " <face_det.kmodel> <det_thres> <nms_thres>"
                 " <face_recg.kmodel> <max_faces> <recg_thres> <db_dir> [debug_mode]\n";
}

int main(int argc, char **argv)
{
    if (argc != 8 && argc != 9)
    {
        print_usage(argv[0]);
        return 1;
    }

    const char *detect_model = argv[1];
    const float detect_threshold = std::atof(argv[2]);
    const float nms_threshold = std::atof(argv[3]);
    const char *recognition_model = argv[4];
    const int max_faces = std::atoi(argv[5]);
    const float recognition_threshold = std::atof(argv[6]);
    char *database_dir = argv[7];
    const int debug_mode = argc == 9 ? std::atoi(argv[8]) : 0;
    mkdir(database_dir, 0777);

    int shmid = -1;
    face_access_shared_t *shared = wait_for_shared_state(shmid);
    while (!face_atomic_load_u32(&shared->ready) &&
           !face_atomic_load_u32(&shared->stop_requested))
        usleep(10000);

    const uint32_t frame_size = shared->frames[0].data_size;
    k_u64 local_paddr = 0;
    void *local_vaddr = nullptr;
    if (kd_mpi_sys_mmz_alloc(&local_paddr, &local_vaddr, "face_ai", "anonymous",
                             frame_size) != K_SUCCESS)
    {
        std::cerr << "face_ai: cannot allocate local MMZ input" << std::endl;
        lwp_shmdt(shared);
        return 1;
    }

    void *slot_vaddr[FACE_ACCESS_FRAME_SLOTS] = {nullptr};
    for (uint32_t i = 0; i < FACE_ACCESS_FRAME_SLOTS; ++i)
    {
        slot_vaddr[i] = kd_mpi_sys_mmap(shared->frames[i].physical_address,
                                        shared->frames[i].data_size);
        if (slot_vaddr[i] == nullptr)
        {
            std::cerr << "face_ai: cannot map capture frame slot " << i << std::endl;
            face_atomic_store_u32(&shared->stop_requested, 1);
            for (uint32_t j = 0; j < i; ++j)
                kd_mpi_sys_munmap(slot_vaddr[j], shared->frames[j].data_size);
            kd_mpi_sys_mmz_free(local_paddr, nullptr);
            lwp_shmdt(shared);
            return 1;
        }
    }

    FaceDetection detector(detect_model, detect_threshold, nms_threshold,
                           {static_cast<int>(shared->frames[0].channels),
                            static_cast<int>(shared->frames[0].height),
                            static_cast<int>(shared->frames[0].width)},
                           reinterpret_cast<uintptr_t>(local_vaddr),
                           static_cast<uintptr_t>(local_paddr), debug_mode);
    FaceRecognition recognizer(recognition_model, max_faces, recognition_threshold,
                               {static_cast<int>(shared->frames[0].channels),
                                static_cast<int>(shared->frames[0].height),
                                static_cast<int>(shared->frames[0].width)},
                               reinterpret_cast<uintptr_t>(local_vaddr),
                               static_cast<uintptr_t>(local_paddr), debug_mode);
    recognizer.database_init(database_dir);

    std::cout << "face_ai ready, database entries=" << recognizer.valid_register_face_ << std::endl;
    uint32_t last_command_sequence = 0;
    uint32_t heartbeat_divider = 0;
    std::string registration_name;
    std::vector<float> registration_sum;
    int registration_samples = 0;
    constexpr int registration_sample_target = 5;

    while (!face_atomic_load_u32(&shared->stop_requested))
    {
        const uint32_t command_sequence = face_atomic_load_u32(&shared->command_sequence);
        if (command_sequence != last_command_sequence &&
            shared->command.type == FACE_COMMAND_RESET_DB)
        {
            registration_name.clear();
            registration_sum.clear();
            registration_samples = 0;
            recognizer.database_reset(database_dir);
            face_access_result_t reset{};
            reset.status = FACE_STATUS_DB_RESET;
            std::strncpy(reset.name, "database reset", sizeof(reset.name) - 1);
            reset.completed_at_ms = monotonic_ms();
            publish_result(shared, reset);
            last_command_sequence = command_sequence;
        }
        else if (command_sequence != last_command_sequence &&
                 shared->command.type == FACE_COMMAND_REGISTER)
        {
            registration_name = shared->command.name;
            registration_sum.assign(recognizer.feature_length(), 0.0f);
            registration_samples = 0;
            last_command_sequence = command_sequence;
            std::cout << "registration started for " << registration_name
                      << ", keep looking at the camera" << std::endl;
        }
        else if (command_sequence != last_command_sequence &&
                 shared->command.type == FACE_COMMAND_STOP)
        {
            face_atomic_store_u32(&shared->stop_requested, 1);
            break;
        }

        int selected = -1;
        face_access_frame_slot_t snapshot{};
        for (uint32_t i = 0; i < FACE_ACCESS_FRAME_SLOTS; ++i)
        {
            if (face_atomic_cas_u32(&shared->frames[i].state,
                                    FACE_SLOT_READY, FACE_SLOT_READING))
            {
                selected = static_cast<int>(i);
                snapshot.sequence = shared->frames[i].sequence;
                snapshot.physical_address = shared->frames[i].physical_address;
                snapshot.data_size = shared->frames[i].data_size;
                snapshot.width = shared->frames[i].width;
                snapshot.height = shared->frames[i].height;
                snapshot.channels = shared->frames[i].channels;
                snapshot.captured_at_ms = shared->frames[i].captured_at_ms;
                std::memcpy(local_vaddr, slot_vaddr[i], frame_size);
                face_atomic_store_u32(&shared->frames[i].state, FACE_SLOT_FREE);
                break;
            }
        }
        if (selected < 0)
        {
            usleep(1000);
            if (++heartbeat_divider >= 1000)
            {
                __atomic_add_fetch(&shared->ai_heartbeat, 1, __ATOMIC_RELAXED);
                heartbeat_divider = 0;
            }
            continue;
        }

        std::vector<FaceDetectionInfo> faces;
        detector.pre_process();
        detector.inference();
        detector.post_process({static_cast<int>(snapshot.width),
                               static_cast<int>(snapshot.height)}, faces);

        if (faces.empty())
        {
            face_access_result_t none{};
            none.frame_sequence = snapshot.sequence;
            none.status = FACE_STATUS_NONE;
            none.captured_at_ms = snapshot.captured_at_ms;
            none.completed_at_ms = monotonic_ms();
            none.latency_ms = static_cast<uint32_t>(none.completed_at_ms - none.captured_at_ms);
            publish_result(shared, none);
            continue;
        }

        auto largest = std::max_element(faces.begin(), faces.end(),
                                        [](const FaceDetectionInfo &a, const FaceDetectionInfo &b) {
                                            return a.bbox.w * a.bbox.h < b.bbox.w * b.bbox.h;
                                        });
        recognizer.pre_process(largest->sparse_kps.points);
        recognizer.inference();

        if (!registration_name.empty())
        {
            const bool face_large_enough = largest->bbox.w >= 100 && largest->bbox.h >= 100;
            std::vector<float> feature;
            if (face_large_enough && recognizer.current_feature(feature))
            {
                for (size_t i = 0; i < feature.size(); ++i)
                    registration_sum[i] += feature[i];
                ++registration_samples;
                std::cout << "registration sample " << registration_samples << '/'
                          << registration_sample_target << std::endl;
            }

            if (registration_samples >= registration_sample_target)
            {
                float norm = 0.0f;
                for (float value : registration_sum)
                    norm += value * value;
                norm = std::sqrt(norm);
                if (norm > 0.0f)
                {
                    for (float &value : registration_sum)
                        value /= norm;
                }

                face_access_result_t registered{};
                registered.frame_sequence = snapshot.sequence;
                registered.status = recognizer.database_insert_feature_named(
                                        database_dir, registration_name.c_str(),
                                        registration_sum.data(),
                                        static_cast<int>(registration_sum.size()))
                                        ? FACE_STATUS_REGISTERED
                                        : FACE_STATUS_ERROR;
                registered.x = static_cast<int32_t>(largest->bbox.x);
                registered.y = static_cast<int32_t>(largest->bbox.y);
                registered.width = static_cast<int32_t>(largest->bbox.w);
                registered.height = static_cast<int32_t>(largest->bbox.h);
                registered.captured_at_ms = snapshot.captured_at_ms;
                registered.completed_at_ms = monotonic_ms();
                registered.latency_ms = static_cast<uint32_t>(registered.completed_at_ms - registered.captured_at_ms);
                std::strncpy(registered.name, registration_name.c_str(), sizeof(registered.name) - 1);
                publish_result(shared, registered);
                registration_name.clear();
                registration_sum.clear();
                registration_samples = 0;
            }
        }
        else
        {
            FaceRecognitionInfo recognition{};
            recognizer.database_search(recognition);
            face_access_result_t result{};
            fill_result(result, snapshot, *largest, recognition, recognition_threshold);
            publish_result(shared, result);
        }
        __atomic_add_fetch(&shared->ai_heartbeat, 1, __ATOMIC_RELAXED);
    }

    for (uint32_t i = 0; i < FACE_ACCESS_FRAME_SLOTS; ++i)
        kd_mpi_sys_munmap(slot_vaddr[i], shared->frames[i].data_size);
    kd_mpi_sys_mmz_free(local_paddr, nullptr);
    lwp_shmdt(shared);
    return 0;
}
