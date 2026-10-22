#include <juce_audio_utils/juce_audio_utils.h>
#include <chrono>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool passed, const char* message) {
    if (!passed) throw std::runtime_error(message);
}
void checkRateAndBlockSizes(juce::AudioPluginInstance& plugin, const juce::String& wrapperName) {
    for (double sampleRate : {44100.0, 48000.0, 96000.0}) {
        for (int blockSize : {64, 127, 256, 1024}) {
            plugin.releaseResources();
            plugin.setPlayConfigDetails(0, 2, sampleRate, blockSize);
            plugin.prepareToPlay(sampleRate, blockSize);
            juce::AudioBuffer<float> block(2, blockSize);
            juce::MidiBuffer events;
            const int offset = blockSize / 3;
            events.addEvent(juce::MidiMessage::noteOn(1, 60, 0.7f), offset);
            double energy = 0;
            for (int index = 0; index < 24; ++index) {
                block.clear();
                plugin.processBlock(block, events);
                if (index == 0)
                    require(block.getMagnitude(0, 0, offset) == 0.0f, "MIDI onset leaked before its host sample offset");
                events.clear();
                for (int channel = 0; channel < 2; ++channel)
                    for (int sample = 0; sample < blockSize; ++sample) {
                        const float value = block.getSample(channel, sample);
                        require(std::isfinite(value) && std::abs(value) <= 1.001f, "Rate/block matrix produced invalid audio");
                        energy += double(value) * value;
                    }
            }
            require(energy > 0.000001, "Rate/block matrix produced no note audio");
            events.addEvent(juce::MidiMessage::allSoundOff(1), 0);
            plugin.processBlock(block, events);
            require(block.getMagnitude(0, blockSize) < 0.0001f, "Rate/block matrix panic did not silence audio");
            std::cout << "PASS: actual " << wrapperName << " at " << sampleRate << " Hz / " << blockSize << " samples\n";
        }
    }
}

juce::AudioBuffer<float> renderAudition(juce::AudioPluginInstance& plugin, const juce::File& artifacts, const juce::String& wrapperSlug) {
    constexpr double sampleRate = 48000;
    constexpr int blockSize = 256, totalFrames = 48000 * 8;
    plugin.releaseResources();
    plugin.setPlayConfigDetails(0, 2, sampleRate, blockSize);
    plugin.prepareToPlay(sampleRate, blockSize);
    juce::AudioBuffer<float> rendered(2, totalFrames), block(2, blockSize);
    for (int frame = 0; frame < totalFrames; frame += blockSize) {
        juce::MidiBuffer events;
        const int count = std::min(blockSize, totalFrames - frame);
        for (int section = 0; section < 3; ++section) {
            const int start = section * 96000, stop = start + 72000;
            const int root = std::array<int, 3>{48, 50, 53}[size_t(section)];
            for (int interval : {0, 7, 12}) {
                if (start >= frame && start < frame + count)
                    events.addEvent(juce::MidiMessage::noteOn(1, root + interval, 0.55f), start - frame);
                if (stop >= frame && stop < frame + count)
                    events.addEvent(juce::MidiMessage::noteOff(1, root + interval), stop - frame);
            }
        }
        block.clear();
        plugin.processBlock(block, events);
        for (int channel = 0; channel < 2; ++channel)
            rendered.copyFrom(channel, frame, block, channel, 0, count);
    }
    const auto file = artifacts.getChildFile("UFO-" + wrapperSlug + "-audition.wav");
    juce::TemporaryFile temporary(file);
    {
        auto stream = temporary.getFile().createOutputStream();
        require(stream != nullptr, "Cannot create audition WAV");
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.release(), sampleRate, 2, 24, {}, 0));
        require(writer != nullptr && writer->writeFromAudioSampleBuffer(rendered, 0, rendered.getNumSamples()), "Cannot write audition WAV");
    }
    require(temporary.overwriteTargetFileWithTemporary(), "Cannot finalize audition WAV");
    std::cout << "Rendered actual-wrapper audition: " << file.getFullPathName() << '\n';
    return rendered;
}

class AuditionPlayback final : public juce::AudioIODeviceCallback {
public:
    explicit AuditionPlayback(const juce::AudioBuffer<float>& audio) : source(audio) {}
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override { increment = 48000.0 / device->getCurrentSampleRate(); }
    void audioDeviceStopped() override {}
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int channels, int frames,
                                         const juce::AudioIODeviceCallbackContext&) override {
        for (int sample = 0; sample < frames; ++sample) {
            const bool available = position < source.getNumSamples();
            const int index = std::min(int(position), source.getNumSamples() - 1);
            const int next = std::min(index + 1, source.getNumSamples() - 1);
            const float blend = float(position - index);
            for (int channel = 0; channel < channels; ++channel) if (outputs[channel]) {
                const int sourceChannel = channel % source.getNumChannels();
                outputs[channel][sample] = available ? source.getSample(sourceChannel, index) * (1 - blend) + source.getSample(sourceChannel, next) * blend : 0.0f;
            }
            if (available) position += increment;
        }
        if (position >= source.getNumSamples()) finished.store(true);
    }
    std::atomic<bool> finished{false};
private:
    const juce::AudioBuffer<float>& source;
    double position = 0, increment = 1;
};

void probeDeviceAndMaybeListen(const juce::AudioBuffer<float>& audio, const juce::File& artifacts, bool requireDevice, bool listen) {
    juce::AudioDeviceManager devices;
    const auto error = devices.initialise(0, 2, nullptr, true);
    auto* active = devices.getCurrentAudioDevice();
    const bool usable = error.isEmpty() && active != nullptr && active->getActiveOutputChannels().countNumberOfSetBits() >= 2
        && std::isfinite(active->getCurrentSampleRate()) && active->getCurrentSampleRate() > 0;
    juce::String report = "Physical listening verdict: PENDING human assessment\n";
    report += "Audio-device open: " + (usable ? juce::String("success") : error.isNotEmpty() ? error : juce::String("no usable stereo output")) + "\n";
    if (auto* device = devices.getCurrentAudioDevice()) {
        report += "Device: " + device->getName() + "\n";
        report += "Sample rate: " + juce::String(device->getCurrentSampleRate()) + "\n";
        report += "Buffer samples: " + juce::String(device->getCurrentBufferSizeSamples()) + "\n";
        report += "Output latency samples: " + juce::String(device->getOutputLatencyInSamples()) + "\n";
        report += "Active output channels: " + juce::String(device->getActiveOutputChannels().countNumberOfSetBits()) + "\n";
    }
    require(artifacts.getChildFile("audio-device.txt").replaceWithText(report), "Cannot write audio device report");
    std::cout << report;
    if (requireDevice || listen) require(usable, "No usable stereo audio output device; offline rendering passed but device listening is blocked");
    if (listen) {
        AuditionPlayback playback(audio);
        devices.addAudioCallback(&playback);
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000;
        while (!playback.finished.load() && juce::Time::getMillisecondCounterHiRes() < deadline) juce::Thread::sleep(20);
        if (playback.finished.load()) {
            auto* device = devices.getCurrentAudioDevice();
            juce::Thread::sleep(int(1000.0 * (device->getOutputLatencyInSamples() + device->getCurrentBufferSizeSamples()) / device->getCurrentSampleRate()) + 50);
        }
        devices.removeAudioCallback(&playback);
        require(playback.finished.load(), "Audio output callback did not consume the complete audition");
        std::cout << "PLAYED: complete audition delivered to the audio-device callback. A human must assess what was heard.\n";
        report += "Audition delivered to output callback: complete\n";
        require(artifacts.getChildFile("audio-device.txt").replaceWithText(report), "Cannot update listening report");
    }
    devices.closeAudioDevice();
}
}

// This loads the built wrapper, rather than linking directly to UFO shared code.
// On macOS --au resolves the temporarily registered AUv2 component. The default
// VST3 mode is portable and also exercises the actual Linux wrapper.
int main(int argc, char** argv) {
    try {
        require(argc >= 2, "Usage: ufo_vst3_host_tests <UFO.vst3|UFO.component> [artifact-directory] [--au] [--probe-device] [--require-device] [--listen]");
        bool probeDevice=false, requireDevice=false, listen=false, audioUnit=false;
        juce::String artifactPath;
        for(int i=2;i<argc;++i) {
            const auto option=juce::String::fromUTF8(argv[i]);
            if(option=="--au")audioUnit=true;
            else if(option=="--probe-device")probeDevice=true;
            else if(option=="--require-device"){probeDevice=true;requireDevice=true;}
            else if(option=="--listen"){probeDevice=true;requireDevice=true;listen=true;}
            else if(!option.startsWith("--") && artifactPath.isEmpty())artifactPath=option;
            else throw std::runtime_error("Unknown host-test option: "+option.toStdString());
        }
        juce::ScopedJuceInitialiser_GUI initialise;
        const juce::File bundle(juce::String::fromUTF8(argv[1]));
        require(bundle.isDirectory(), "Plugin bundle does not exist");
        require(bundle.hasFileExtension(audioUnit ? ".component" : ".vst3"), "Bundle extension does not match the selected wrapper format");
        const juce::String wrapperName=audioUnit ? "AU" : "VST3",wrapperSlug=audioUnit ? "au" : "vst3";
        const juce::File artifacts = artifactPath.isNotEmpty()
            ? juce::File::getCurrentWorkingDirectory().getChildFile(artifactPath)
            : juce::File::getCurrentWorkingDirectory();
        require(artifacts.createDirectory().wasOk(), "Cannot create host validation artifact directory");

        std::unique_ptr<juce::AudioPluginFormat> format;
        if(audioUnit) {
           #if JUCE_MAC && JUCE_PLUGINHOST_AU
            format=std::make_unique<juce::AudioUnitPluginFormat>();
           #else
            throw std::runtime_error("Audio Unit hosting requires the macOS host-test build");
           #endif
        } else format=std::make_unique<juce::VST3PluginFormat>();
        std::cout << "Validating actual " << wrapperName << " wrapper: " << bundle.getFullPathName() << '\n';
        juce::OwnedArray<juce::PluginDescription> descriptions;
        format->findAllTypesForFile(descriptions, bundle.getFullPathName());
        require(descriptions.size() > 0, "Wrapper scanner found no plugin classes");
        const juce::PluginDescription* description = nullptr;
        for (auto* candidate : descriptions)
            if (candidate->name == "UFO" && candidate->isInstrument) { description = candidate; break; }
        require(description != nullptr, "Scanner did not find the UFO instrument class");
        if(audioUnit) {
            require(description->fileOrIdentifier=="AudioUnit:Synths/aumu,Ufo1,MpAu", "Scanner resolved an unexpected AU type/subtype/manufacturer");
            std::cout << "Registered AU identity: " << description->fileOrIdentifier << '\n';
        }
        juce::String error;
        auto plugin = format->createInstanceFromDescription(*description, 48000.0, 256, error);
        if (plugin == nullptr) throw std::runtime_error("Cannot instantiate UFO " + wrapperName.toStdString() + ": " + error.toStdString());
        plugin->setPlayConfigDetails(0, 2, 48000.0, 256);
        plugin->prepareToPlay(48000.0, 256);
        require(plugin->getTotalNumOutputChannels() == 2, "Wrapper host did not negotiate stereo output");

        juce::AudioBuffer<float> buffer(2, 256);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 69, 0.8f), 128);
        plugin->processBlock(buffer, midi);
        require(buffer.getMagnitude(0, 0, 128) == 0.0f, "Wrapper MIDI sample offset was not preserved");
        require(buffer.getMagnitude(0, 128, 128) > 0.0f, "Plugin wrapper produced no note audio");
        juce::AudioProcessorParameter* outputParameter = nullptr;
        for (auto* parameter : plugin->getParameters())
            if (parameter->getName(128) == "Output") { outputParameter = parameter; break; }
        require(outputParameter != nullptr, "Wrapper host did not expose the Output parameter");
        outputParameter->setValueNotifyingHost(0.31f);
        midi.clear();
        buffer.clear();
        plugin->processBlock(buffer, midi); // Apply queued host parameter changes.
        juce::MemoryBlock state;
        plugin->getStateInformation(state);
        require(state.getSize() > 0, "Wrapper host state serialization returned no data");
        outputParameter->setValueNotifyingHost(0.81f);
        midi.clear();
        buffer.clear();
        plugin->processBlock(buffer, midi);
        plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        midi.clear();
        buffer.clear();
        plugin->processBlock(buffer, midi);
        require(std::abs(outputParameter->getValue() - 0.31f) < 0.001f,
                "Wrapper host parameter state did not round-trip");
        midi.clear();
        midi.addEvent(juce::MidiMessage::allSoundOff(1), 0);
        for (int note = 48; note < 62; ++note)
            midi.addEvent(juce::MidiMessage::noteOn(1, note, 0.7f), 0);
        plugin->processBlock(buffer, midi);
        midi.clear();
        double energy = 0.0;
        constexpr int blocks = 1000;
        const auto start = std::chrono::steady_clock::now();
        for (int block = 0; block < blocks; ++block) {
            buffer.clear();
            plugin->processBlock(buffer, midi);
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
                    const float value = buffer.getSample(channel, sample);
                    require(std::isfinite(value) && std::abs(value) <= 1.001f,
                            "Plugin audio was not finite and bounded");
                    energy += static_cast<double>(value) * value;
                }
            midi.clear();
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        require(energy > 0.0, "Plugin wrapper became silent during sustained rendering");
        const double audioSeconds = static_cast<double>(blocks * 256) / 48000.0;
        std::cout << wrapperName << " 14-voice render: " << audioSeconds << " audio seconds in " << elapsed
                  << " wall seconds (" << elapsed / audioSeconds * 100.0
                  << "% of one core; diagnostic, not a realtime deadline guarantee)\n";

        {
            std::unique_ptr<juce::AudioProcessorEditor> editor(plugin->createEditor());
            require(editor != nullptr && editor->getWidth() > 0 && editor->getHeight() > 0,
                    "Wrapper host could not create the native editor");
            // The wrapper embeds the instrument UI in a native child. Attach
            // a real peer and capture
            // that native window after dispatching paints and plugin run-loop work.
            editor->setVisible(false);
            editor->setTopLeftPosition(20, 20);
            editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);
            editor->setVisible(true);
            editor->toFront(false);
            juce::Image image;
            juce::String captureError;
            juce::Timer::callAfterDelay(750, [&] {
                try {
                    if (auto* peer = editor->getPeer())
                        image = juce::createSnapshotOfNativeWindow(peer->getNativeHandle());
                    else
                        captureError = "Native editor peer was not created";
                } catch (const std::exception& exception) {
                    captureError = exception.what();
                }
                juce::MessageManager::getInstance()->stopDispatchLoop();
            });
            juce::MessageManager::getInstance()->runDispatchLoop();
            editor->setVisible(false);
            require(captureError.isEmpty(), captureError.toRawUTF8());
            require(image.isValid() && image.getWidth() > 0 && image.getHeight() > 0,
                    "Native plugin window capture failed");
            int samples = 0, brightPixels = 0, coloredPixels = 0;
            for (int y = 0; y < image.getHeight(); y += 4)
                for (int x = 0; x < image.getWidth(); x += 4) {
                    const auto color = image.getPixelAt(x, y);
                    const int brightest = std::max({int(color.getRed()), int(color.getGreen()), int(color.getBlue())});
                    const int darkest = std::min({int(color.getRed()), int(color.getGreen()), int(color.getBlue())});
                    ++samples;
                    if (brightest > 100) ++brightPixels;
                    if (brightest > 80 && brightest - darkest > 30) ++coloredPixels;
                }
            require(brightPixels > samples / 1000 && coloredPixels > samples / 5000,
                    "Native plugin editor capture is blank or has no rendered instrument controls");
            std::cout << "Native editor capture: " << image.getWidth() << "x" << image.getHeight()
                      << ", sampled bright pixels=" << brightPixels << ", colored pixels=" << coloredPixels << '\n';
            const auto screenshot = artifacts.getChildFile("UFO-" + wrapperSlug + "-host-editor.png");
            juce::FileOutputStream stream(screenshot);
            require(stream.openedOk(), "Cannot open plugin editor screenshot");
            stream.setPosition(0);
            require(stream.truncate().wasOk(), "Cannot truncate plugin editor screenshot");
            juce::PNGImageFormat png;
            require(png.writeImageToStream(image, stream), "Cannot write plugin editor screenshot");
            stream.flush();
        }
        midi.addEvent(juce::MidiMessage::allSoundOff(1), 0);
        buffer.clear();
        plugin->processBlock(buffer, midi);
        require(buffer.getMagnitude(0, 256) < 0.0001f, "Plugin all-sound-off did not clear audio");
        checkRateAndBlockSizes(*plugin,wrapperName);
        const auto audition=renderAudition(*plugin,artifacts,wrapperSlug);
        if(probeDevice)probeDeviceAndMaybeListen(audition,artifacts,requireDevice,listen);
        const auto notes=artifacts.getChildFile("LISTENING-NOTES.md");
        if(!notes.existsAsFile())require(notes.replaceWithText("# UFO listening and DAW observations\n\nStatus: PENDING human assessment. Automated offline renders do not establish audibility or DAW compatibility.\n\n- Mac model / macOS / CPU architecture:\n- Output device / sample rate / buffer size:\n- Audition heard in both channels, no unwanted clicks/dropouts:\n- DAW name/version; instrument scan/load:\n- MIDI keyboard, sustain/pitch bend and patch recall:\n- GPU visualization and long-source seeks during playback:\n- Project save/reopen and offline bounce:\n- Result / issues / log paths:\n"),"Cannot create manual observation template");
        plugin->releaseResources();
        plugin.reset();
        std::cout << "PASS: actual " << wrapperName << " scan/load, stereo negotiation, MIDI timing, audio, state, native editor and all-sound-off\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
