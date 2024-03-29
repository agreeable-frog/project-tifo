#include <benchmark/benchmark.h>
#include "io_png.hh"
#include "filter.cuh"

constexpr int niteration = 40;

void BM_Rendering_gpu(benchmark::State& st)
{
    int width;
    int height;
    const auto image_ = read_png("../../resources/forest_fHD.png", &width, &height);
    const auto image = flatten(image_, width * 3, height);
    for (int i = 0; i < height; free(image_[i++]));
    free(image_);
    for (auto _ : st)
        oil_filter(image, width, height);

    free(image);
    st.counters["frame_rate"] = benchmark::Counter(st.iterations(), benchmark::Counter::kIsRate);
}

BENCHMARK(BM_Rendering_gpu)
->Unit(benchmark::kMillisecond)
->UseRealTime()
->Iterations(niteration);

BENCHMARK_MAIN();
