import glob
import os
import re
import subprocess
import sys

os.chdir("cognac/")

successes = 0
failures = 0
errors = 0
crashes = 0


def test(file: str, process: subprocess.Popen, pad_length: int, is_kill: bool):
    global successes
    global failures
    global errors
    global crashes
    if process.poll() is None:
        return False
    print("Testing", file.ljust(pad_length), end=" :: ")
    exit_code = process.returncode
    out: bytes = process.stdout.read() + process.stderr.read() + \
        f"\nProgram exited with code {exit_code}\n".encode()
    if is_kill:
        print("\x1b[35mINTERRUPT\x1b[0m", end=" ")
    elif exit_code != 0:
        crashes += 1
        print("\x1b[31mCRASH\x1b[0m", end=" ")
    elif b"ERROR" in out:
        errors += 1
        print("\x1b[31mERROR\x1b[0m", end=" ")
    elif b"FAIL" in out:
        failures += 1
        print("\x1b[31mFAIL\x1b[0m", end=" ")
    elif b"PASS" in out:
        successes += 1
        print("\x1b[32mPASS\x1b[0m", end=" ")
    else:
        print("NO STATUS", end=" ")
    with open(file.replace(".cog", ".log"), "wb") as f:
        f.write(out)
    if (m := re.search(r"undefined: (.+?)\n", out.decode(), re.I)):
        print("requires", m.group(1), end="")
    print()
    return True


is_profiling = len(sys.argv) > 1
test_files = sorted(glob.glob("tests/*.cog"))
if is_profiling:
    raise NotImplementedError
    test_commands = [["xctrace", "record",
                      "--output", "traces/",
                      "--template", "Time Profiler",
                      "--time-limit", "10s",
                      "--launch", "--", "../cogni", file]
                     for file in test_files]

else:
    test_commands = [["../cogni", file] for file in test_files]
test_processes = [subprocess.Popen(
    command, stderr=subprocess.PIPE, stdout=subprocess.PIPE)
    for command in test_commands]
file2proc = dict(zip(test_files, [[p, False] for p in test_processes]))
pl = max(map(len, test_files))
try:
    while any(not q for p, q in file2proc.values()):
        for f, (p, q) in file2proc.items():
            if not q:
                file2proc[f][1] = test(f, p, pl, False)
except KeyboardInterrupt:
    for f, (p, q) in file2proc.items():
        if not q:
            p.kill()
            test(f, p, pl, True)

print("Successes:", successes)
print(" Failures:", failures)
print("   Errors:", errors)
print("  Crashes:", crashes)
