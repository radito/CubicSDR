// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "FFTDataDistributor.h"
#include "VisualProcessor.h"

#include <memory>
#include <string>

namespace {

struct Value {
    int number = 0;
};

class DirectDistributor : public VisualDataDistributor<Value> {};
class CopyDistributor : public VisualDataReDistributor<Value> {
public:
    CopyDistributor() = default;
};

class ObservedThread : public IOThread {
public:
    void run() override {
        ran = true;
    }

    void onBindInput(std::string name, ThreadQueueBasePtr queue) override {
        inputName = std::move(name);
        inputBinding = std::move(queue);
    }

    void onBindOutput(std::string name, ThreadQueueBasePtr queue) override {
        outputName = std::move(name);
        outputBinding = std::move(queue);
    }

    bool isStopping() const {
        return stopping.load();
    }

    bool ran = false;
    std::string inputName;
    std::string outputName;
    ThreadQueueBasePtr inputBinding;
    ThreadQueueBasePtr outputBinding;
};

} // namespace

CUBIC_TEST(io_thread_tracks_bindings_lifecycle_and_termination) {
    ObservedThread thread;
    auto input = std::make_shared<ThreadBlockingQueue<int>>();
    auto output = std::make_shared<ThreadBlockingQueue<int>>();

    CUBIC_REQUIRE(!thread.isTerminated());
    thread.setInputQueue("samples", input);
    thread.setOutputQueue("audio", output);
    CUBIC_REQUIRE(thread.getInputQueue("samples") == input);
    CUBIC_REQUIRE(thread.getOutputQueue("audio") == output);
    CUBIC_REQUIRE(thread.inputName == "samples");
    CUBIC_REQUIRE(thread.outputName == "audio");

    thread.threadMain();
    CUBIC_REQUIRE(thread.ran);
    CUBIC_REQUIRE(thread.isTerminated());
    CUBIC_REQUIRE(thread.isStopping());
}

CUBIC_TEST(visual_distributor_shares_input_with_every_output) {
    DirectDistributor distributor;
    auto input = std::make_shared<ThreadBlockingQueue<std::shared_ptr<Value>>>();
    auto firstOutput = std::make_shared<ThreadBlockingQueue<std::shared_ptr<Value>>>();
    auto secondOutput = std::make_shared<ThreadBlockingQueue<std::shared_ptr<Value>>>();
    firstOutput->set_max_num_items(2);
    secondOutput->set_max_num_items(2);
    distributor.setInput(input);
    distributor.attachOutput(firstOutput);
    distributor.attachOutput(secondOutput);

    auto value = std::make_shared<Value>();
    value->number = 73;
    CUBIC_REQUIRE(input->try_push(value));
    distributor.run();

    std::shared_ptr<Value> first;
    std::shared_ptr<Value> second;
    CUBIC_REQUIRE(firstOutput->try_pop(first));
    CUBIC_REQUIRE(secondOutput->try_pop(second));
    CUBIC_REQUIRE(first == value);
    CUBIC_REQUIRE(second == value);
}

CUBIC_TEST(visual_redistributor_deep_copies_input_and_flushes_queues) {
    CopyDistributor distributor;
    auto input = std::make_shared<ThreadBlockingQueue<std::shared_ptr<Value>>>();
    auto output = std::make_shared<ThreadBlockingQueue<std::shared_ptr<Value>>>();
    distributor.setInput(input);
    distributor.attachOutput(output);

    auto value = std::make_shared<Value>();
    value->number = 9;
    CUBIC_REQUIRE(input->try_push(value));
    distributor.run();

    std::shared_ptr<Value> copy;
    CUBIC_REQUIRE(output->try_pop(copy));
    CUBIC_REQUIRE(copy != value);
    CUBIC_REQUIRE(copy->number == 9);

    CUBIC_REQUIRE(input->try_push(value));
    CUBIC_REQUIRE(output->try_push(copy));
    distributor.flushQueues();
    CUBIC_REQUIRE(input->empty());
    CUBIC_REQUIRE(output->empty());
}

CUBIC_TEST(fft_distributor_frames_samples_and_preserves_metadata) {
    FFTDataDistributor distributor;
    distributor.setFFTSize(4);
    distributor.setLinesPerSecond(8);
    CUBIC_REQUIRE(distributor.getLinesPerSecond() == 8);

    auto input = std::make_shared<DemodulatorThreadInputQueue>();
    auto output = std::make_shared<DemodulatorThreadInputQueue>();
    output->set_max_num_items(4);
    distributor.setInput(input);
    distributor.attachOutput(output);

    auto samples = std::make_shared<DemodulatorThreadIQData>();
    samples->frequency = 101700000;
    samples->sampleRate = 32;
    samples->data.resize(8);
    for (size_t i = 0; i < samples->data.size(); ++i) {
        samples->data[i].real = static_cast<float>(i);
        samples->data[i].imag = -static_cast<float>(i);
    }
    CUBIC_REQUIRE(input->try_push(samples));
    distributor.run();

    CUBIC_REQUIRE(output->size() == 2);
    DemodulatorThreadIQDataPtr first;
    DemodulatorThreadIQDataPtr second;
    CUBIC_REQUIRE(output->try_pop(first));
    CUBIC_REQUIRE(output->try_pop(second));
    CUBIC_REQUIRE(first->frequency == samples->frequency);
    CUBIC_REQUIRE(first->sampleRate == samples->sampleRate);
    CUBIC_REQUIRE(first->data.size() == 4);
    CUBIC_REQUIRE(second->data.size() == 4);
    CUBIC_REQUIRE(first->data.front().real == 0.0f);
    CUBIC_REQUIRE(second->data.front().real == 4.0f);
}
