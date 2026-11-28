# Running the Benchmarks

This page is for anyone who wants to measure the SDK on their own machine. You need no knowledge of the SDK code. The run builds a test program, runs every benchmark and writes one CSV file.

## What you need

A checkout of the SDK that builds, with the tools from the [Getting Started](getting-started.md) guide. Configure the build folder with the preset for your system as that guide shows. The benchmarks need a Release build and the `rextest_benchmark_all` target builds it for you.

## Run

Open the same shell you use to build the SDK, which is the x64 Developer PowerShell on Windows, and run the commands for your system from the folder of the checkout.

=== "Windows"

    ```
    cmake --preset win-amd64
    cmake --build out/build/win-amd64 --config Release --target rextest_benchmark_all
    ```

=== "Linux"

    ```
    cmake --preset linux-amd64
    cmake --build out/build/linux-amd64 --config Release --target rextest_benchmark_all
    ```

On an ARM machine use the `linux-arm64` preset and its folder. The first run builds the SDK and the test programs, then the 24 benchmark cases run. The first run takes about 15 to 25 minutes on a 6 core machine, most of it the build. A later run on the same build folder takes about 2 minutes.

Before you run, get clean figures with these steps.

- Close other programs, and stop downloads and updates.
- Plug in a laptop and use the high performance power mode.
- Use a Release build only. A Debug build prints a warning and gives figures that say nothing about the speed of the SDK.
- Run without a debugger attached.
- Leave the machine alone until the run ends.

## The result

The target prints the table of Catch2 while it runs. The last two lines name the CSV file and the number of rows. The file is in the `rextest` folder of the build folder, for example `out/build/win-amd64/rextest/rextest-benchmark-3f9a1c2-Release-20261121T180500Z.csv`. Its name holds the commit, the configuration and the time of the run in UTC, so a new run never replaces an earlier file, and files from many machines can sit in one folder. A matching `.xml` file with the full Catch2 results is beside it.

The CSV file has one header row and one row for each benchmark. Every row repeats the columns of the machine, so files can be joined as they are. The file holds no host name, user name or folder path.

| Column | Meaning |
| --- | --- |
| `schema` | Version of the column layout, 1 for this layout |
| `test_case` | Name of the Catch2 test case |
| `benchmark` | Name of the benchmark inside the test case |
| `samples` | Number of samples taken |
| `iterations` | Calls timed in each sample |
| `mean_ns` | Mean time of one call in nanoseconds |
| `mean_low_ns` | Lower bound of the confidence interval of the mean, in nanoseconds |
| `mean_high_ns` | Upper bound of the confidence interval of the mean, in nanoseconds |
| `std_dev_ns` | Standard deviation in nanoseconds |
| `config` | Build configuration, which is `Release` for useful figures |
| `commit` | Short commit hash of the checkout, empty when the folder is not a git checkout |
| `sdk_version` | Version string of the SDK build |
| `compiler` | Compiler name and version |
| `os` | Operating system name |
| `os_version` | Operating system release |
| `arch` | CPU architecture of the operating system |
| `cpu` | Name of the processor, or `unknown` when the system gives none |
| `logical_cores` | Number of logical cores |
| `memory_mb` | Installed memory in megabytes |
| `run_utc` | Start time of the run in UTC |

A text field that holds a comma or a quote is in double quotes, as spreadsheets expect.
