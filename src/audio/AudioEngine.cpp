#include "audio/AudioEngine.h"
#include "audio/VoiceEncoder.h"
#include "runtime/Profiling.h"

#include <windows.h>

#include <miniaudio.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace Audio
{
    namespace
    {
        constexpr ma_uint32 kPeriodMilliseconds = 10;
        // Samples between checks of the window in front, about 100 ms.
        constexpr ma_uint32 kForegroundCheckSamples = Multiplayer::kVoiceSampleRate / 10;

        bool IsGameInFront()
        {
            const auto window = ::GetForegroundWindow();
            DWORD process = 0;
            return window && ::GetWindowThreadProcessId(window, &process) != 0 && process == ::GetCurrentProcessId();
        }

        struct DeviceDeleter
        {
            void operator()(ma_device* a_device) const
            {
                ma_device_uninit(a_device);
                delete a_device;
            }
        };
        using Device = std::unique_ptr<ma_device, DeviceDeleter>;

        struct Engine
        {
            VoiceEncoder encoder;
            VoiceMixer mixer;
            std::atomic<float> outputGain{ 1.0F };
            std::atomic_bool allowed{ false };
            std::atomic_bool pushToTalk{ false };
            std::atomic_bool inputOpen{ false };
            std::atomic_bool outputOpen{ false };

            std::mutex lock;
            std::condition_variable wake;
            bool pending{ false };
            bool refresh{ true };
            Settings desired;
            std::thread worker;

            std::mutex namesLock;
            std::vector<std::string> inputNames;
            std::vector<std::string> outputNames;

            // The worker's, and the capture thread's for the foreground fields.
            bool contextReady{ false };
            ma_context context{};
            Device capture;
            Device playback;
            std::string captureName;
            std::string playbackName;
            bool foreground{ true };
            ma_uint32 sinceForegroundCheck{ 0 };
        };

        // Never destroyed: a device's thread may still call back while statics are torn down at exit.
        Engine& GetEngine()
        {
            static auto* engine = new Engine();
            return *engine;
        }

        void OnCapture(ma_device* a_device, void*, const void* a_input, ma_uint32 a_frames)
        {
            PROFILE_SCOPE("Voice capture");
            auto& engine = *static_cast<Engine*>(a_device->pUserData);
            engine.sinceForegroundCheck += a_frames;
            if (engine.sinceForegroundCheck >= kForegroundCheckSamples)
            {
                engine.sinceForegroundCheck = 0;
                // A key released while another window had focus never reaches the game.
                engine.foreground = IsGameInFront();
            }
            engine.encoder.Process(static_cast<const float*>(a_input), a_frames,
                engine.allowed.load(std::memory_order_acquire),
                engine.pushToTalk.load(std::memory_order_acquire) && engine.foreground);
        }

        void OnPlayback(ma_device* a_device, void* a_output, const void*, ma_uint32 a_frames)
        {
            PROFILE_SCOPE("Voice playback");
            auto& engine = *static_cast<Engine*>(a_device->pUserData);
            engine.mixer.Render(static_cast<float*>(a_output), a_frames, engine.outputGain.load(std::memory_order_relaxed));
        }

        void OnNotification(const ma_device_notification* a_notification)
        {
            if (a_notification->type != ma_device_notification_type_stopped)
            {
                return;
            }
            auto& engine = *static_cast<Engine*>(a_notification->pDevice->pUserData);
            const bool input = a_notification->pDevice->type == ma_device_type_capture;
            // Also called after a stop the worker asked for, when the flag is already clear.
            if ((input ? engine.inputOpen : engine.outputOpen).exchange(false, std::memory_order_acq_rel))
            {
                logs::warn("Voice {} device stopped by itself; it reopens with the next voice setting change",
                    input ? "input" : "output");
            }
        }

        void ListDevices(Engine& a_engine)
        {
            ma_device_info* playbackInfos = nullptr;
            ma_uint32 playbackCount = 0;
            ma_device_info* captureInfos = nullptr;
            ma_uint32 captureCount = 0;
            if (ma_context_get_devices(&a_engine.context, &playbackInfos, &playbackCount, &captureInfos, &captureCount)
                != MA_SUCCESS)
            {
                logs::warn("Voice could not list audio devices");
                return;
            }
            std::vector<std::string> inputs;
            std::vector<std::string> outputs;
            for (ma_uint32 i = 0; i < captureCount; ++i)
            {
                inputs.emplace_back(captureInfos[i].name);
            }
            for (ma_uint32 i = 0; i < playbackCount; ++i)
            {
                outputs.emplace_back(playbackInfos[i].name);
            }
            logs::info("Voice found {} input and {} output devices", inputs.size(), outputs.size());
            const std::scoped_lock lock{ a_engine.namesLock };
            a_engine.inputNames = std::move(inputs);
            a_engine.outputNames = std::move(outputs);
        }

        std::optional<ma_device_id> FindDevice(Engine& a_engine, ma_device_type a_type, const std::string& a_name)
        {
            if (a_name.empty())
            {
                return std::nullopt;
            }
            ma_device_info* playbackInfos = nullptr;
            ma_uint32 playbackCount = 0;
            ma_device_info* captureInfos = nullptr;
            ma_uint32 captureCount = 0;
            if (ma_context_get_devices(&a_engine.context, &playbackInfos, &playbackCount, &captureInfos, &captureCount)
                == MA_SUCCESS)
            {
                const auto* infos = a_type == ma_device_type_capture ? captureInfos : playbackInfos;
                const auto count = a_type == ma_device_type_capture ? captureCount : playbackCount;
                for (ma_uint32 i = 0; i < count; ++i)
                {
                    if (a_name == infos[i].name)
                    {
                        return infos[i].id;
                    }
                }
            }
            logs::warn("Voice has no {} device named \"{}\"; using the system's default",
                a_type == ma_device_type_capture ? "input" : "output", a_name);
            return std::nullopt;
        }

        Device OpenDevice(Engine& a_engine, ma_device_type a_type, const std::string& a_name)
        {
            const auto id = FindDevice(a_engine, a_type, a_name);
            auto config = ma_device_config_init(a_type);
            config.sampleRate = Multiplayer::kVoiceSampleRate;
            config.periodSizeInMilliseconds = kPeriodMilliseconds;
            config.performanceProfile = ma_performance_profile_low_latency;
            config.notificationCallback = OnNotification;
            config.pUserData = &a_engine;
            if (a_type == ma_device_type_capture)
            {
                config.capture.format = ma_format_f32;
                config.capture.channels = 1;
                config.capture.pDeviceID = id ? &*id : nullptr;
                config.dataCallback = OnCapture;
            }
            else
            {
                config.playback.format = ma_format_f32;
                config.playback.channels = 2;
                config.playback.pDeviceID = id ? &*id : nullptr;
                config.dataCallback = OnPlayback;
            }
            auto* device = new ma_device{};
            if (const auto result = ma_device_init(&a_engine.context, &config, device); result != MA_SUCCESS)
            {
                delete device;
                logs::error("Voice could not open the {} device: {}", a_type == ma_device_type_capture ? "input" : "output",
                    ma_result_description(result));
                return nullptr;
            }
            Device opened{ device };
            if (const auto result = ma_device_start(device); result != MA_SUCCESS)
            {
                logs::error("Voice could not start the {} device: {}",
                    a_type == ma_device_type_capture ? "input" : "output", ma_result_description(result));
                return nullptr;
            }
            logs::info("Voice opened the {} device \"{}\"", a_type == ma_device_type_capture ? "input" : "output",
                a_type == ma_device_type_capture ? device->capture.name : device->playback.name);
            return opened;
        }

        void Apply(Engine& a_engine, const Settings& a_settings)
        {
            a_engine.encoder.SetGain(a_settings.inputGain);
            a_engine.encoder.SetNoiseSuppression(a_settings.noiseSuppression);
            a_engine.encoder.SetVoiceActivation(a_settings.voiceActivation, a_settings.activationLevel);
            a_engine.outputGain.store(a_settings.outputGain, std::memory_order_relaxed);

            // Closed when inactive, for another device, or after stopping by itself; opened again below.
            if (a_engine.capture
                && (!a_settings.active || a_settings.inputDevice != a_engine.captureName
                    || !a_engine.inputOpen.load(std::memory_order_acquire)))
            {
                a_engine.inputOpen.store(false, std::memory_order_release);
                a_engine.capture.reset();
                // After the device's thread stopped, so a spurt in progress ends with an end frame.
                a_engine.encoder.Reset();
            }
            if (a_engine.playback
                && (!a_settings.active || a_settings.outputDevice != a_engine.playbackName
                    || !a_engine.outputOpen.load(std::memory_order_acquire)))
            {
                a_engine.outputOpen.store(false, std::memory_order_release);
                a_engine.playback.reset();
            }
            if (!a_settings.active)
            {
                a_engine.mixer.Clear();
                return;
            }
            if (!a_engine.capture)
            {
                a_engine.captureName = a_settings.inputDevice;
                a_engine.capture = OpenDevice(a_engine, ma_device_type_capture, a_settings.inputDevice);
                a_engine.inputOpen.store(a_engine.capture != nullptr, std::memory_order_release);
            }
            if (!a_engine.playback)
            {
                a_engine.playbackName = a_settings.outputDevice;
                a_engine.playback = OpenDevice(a_engine, ma_device_type_playback, a_settings.outputDevice);
                a_engine.outputOpen.store(a_engine.playback != nullptr, std::memory_order_release);
            }
        }

        void RunWorker(Engine& a_engine)
        {
            PROFILE_THREAD_NAME("Voice devices");
            std::optional<Settings> applied;
            while (true)
            {
                Settings settings;
                bool refresh = false;
                {
                    std::unique_lock lock{ a_engine.lock };
                    a_engine.wake.wait(lock, [&a_engine] { return a_engine.pending; });
                    a_engine.pending = false;
                    settings = a_engine.desired;
                    refresh = std::exchange(a_engine.refresh, false);
                }
                if (!a_engine.contextReady)
                {
                    if (ma_context_init(nullptr, 0, nullptr, &a_engine.context) != MA_SUCCESS)
                    {
                        logs::error("Voice could not initialize audio; voice chat stays silent");
                        return;
                    }
                    a_engine.contextReady = true;
                    logs::info("Voice uses the {} audio backend", ma_get_backend_name(a_engine.context.backend));
                    refresh = true;
                }
                if (refresh)
                {
                    ListDevices(a_engine);
                }
                // A device that stopped by itself is opened again with the settings.
                if (applied != settings || !a_engine.inputOpen.load(std::memory_order_acquire)
                    || !a_engine.outputOpen.load(std::memory_order_acquire))
                {
                    Apply(a_engine, settings);
                    applied = settings;
                }
            }
        }

        void Post(Engine& a_engine, const Settings* a_settings, bool a_refresh)
        {
            {
                const std::scoped_lock lock{ a_engine.lock };
                if (a_settings)
                {
                    a_engine.desired = *a_settings;
                }
                a_engine.refresh = a_engine.refresh || a_refresh;
                a_engine.pending = true;
                if (!a_engine.worker.joinable())
                {
                    // Detached by never joining: it waits for work until the process ends.
                    a_engine.worker = std::thread(RunWorker, std::ref(a_engine));
                }
            }
            a_engine.wake.notify_one();
        }
    }

    void Configure(const Settings& a_settings)
    {
        Post(GetEngine(), &a_settings, false);
    }

    void SetFrameSink(FrameSink a_sink)
    {
        GetEngine().encoder.SetEmit(a_sink);
    }

    void SetTransmission(bool a_allowed, bool a_pushToTalkHeld)
    {
        auto& engine = GetEngine();
        engine.allowed.store(a_allowed, std::memory_order_release);
        engine.pushToTalk.store(a_pushToTalkHeld, std::memory_order_release);
    }

    void PushFrame(const Multiplayer::VoiceFrame& a_frame)
    {
        auto& engine = GetEngine();
        if (engine.outputOpen.load(std::memory_order_acquire))
        {
            engine.mixer.Push(a_frame);
        }
    }

    void SetSpeakers(std::span<const Speaker> a_speakers)
    {
        GetEngine().mixer.SetSpeakers(a_speakers);
    }

    void CopySpeakerLevels(std::vector<SpeakerLevel>& a_levels)
    {
        GetEngine().mixer.CopyLevels(a_levels);
    }

    float GetInputLevel()
    {
        auto& engine = GetEngine();
        return engine.inputOpen.load(std::memory_order_acquire) ? engine.encoder.GetLevel() : 0.0F;
    }

    bool IsTransmitting()
    {
        auto& engine = GetEngine();
        return engine.inputOpen.load(std::memory_order_acquire) && engine.encoder.IsTransmitting();
    }

    bool IsInputOpen()
    {
        return GetEngine().inputOpen.load(std::memory_order_acquire);
    }

    bool IsOutputOpen()
    {
        return GetEngine().outputOpen.load(std::memory_order_acquire);
    }

    void CopyDeviceNames(bool a_capture, std::vector<std::string>& a_names)
    {
        auto& engine = GetEngine();
        const std::scoped_lock lock{ engine.namesLock };
        a_names = a_capture ? engine.inputNames : engine.outputNames;
    }

    void RefreshDevices()
    {
        Post(GetEngine(), nullptr, true);
    }
}
