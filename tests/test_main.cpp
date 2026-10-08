#include "test_framework.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace gctest
{
namespace
{
struct test_failure : std::runtime_error
{
    using std::runtime_error::runtime_error;
};
} // namespace

std::vector<test_case>& registry()
{
    static std::vector<test_case> tests;
    return tests;
}

void fail(const char* file, int line, const std::string& message)
{
    throw test_failure(std::string(file) + ":" + std::to_string(line) + ": " + message);
}
} // namespace gctest

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int failed = 0;
    int run = 0;
    for (const auto& t : gctest::registry())
    {
        if (filter && std::strstr(t.name, filter) == nullptr)
            continue;
        ++run;
        try
        {
            t.fn();
            std::printf("[ ok ] %s\n", t.name);
        }
        catch (const std::exception& e)
        {
            ++failed;
            std::printf("[FAIL] %s\n       %s\n", t.name, e.what());
        }
    }
    std::printf("\n%d run, %d failed\n", run, failed);
    return failed == 0 ? 0 : 1;
}
