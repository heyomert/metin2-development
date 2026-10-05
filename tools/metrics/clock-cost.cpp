// Cost of std::chrono::steady_clock::now() on the server host (server metrics read it 4 + 2 x pulses times per loop
// iteration, server_metrics.h). Build on the VM: c++ -std=c++20 -O2 -o /tmp/clock-cost clock-cost.cpp
#include <chrono>
#include <cstdio>

int main()
{
	using Clock = std::chrono::steady_clock;
	printf("steady_clock period: %lld/%lld s, is_steady=%d\n",
		(long long) Clock::period::num, (long long) Clock::period::den, (int) Clock::is_steady);

	const int N = 1000000;
	for (int run = 0; run < 3; ++run)
	{
		Clock::duration sink{};
		const auto start = Clock::now();
		for (int i = 0; i < N; ++i)
			sink += Clock::now().time_since_epoch();
		const auto end = Clock::now();
		const double ns = std::chrono::duration<double, std::nano>(end - start).count() / N;
		printf("run %d: %.1f ns per now() (checksum %lld)\n", run, ns, (long long) sink.count() % 7);
	}
	return 0;
}
