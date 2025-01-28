import glob
import subprocess
import re
import os

os.chdir("cognac/")

successes = 0
failures = 0
errors = 0
crashes = 0


def test(file: str, process: subprocess.Popen, pad_length: int):
    global successes
    global failures
    global errors
    global crashes
    print("Testing", file.ljust(pad_length), end=" :: ")
    try:
        exit_code = process.wait()
    except KeyboardInterrupt:
        exit_code = "^C"
    out: bytes = process.stdout.read()
    if exit_code == "^C":
        print("\x1b[35mINTERRUPT\x1b[0m", end=" ")
        out += b"^C"
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


test_files = sorted(glob.glob("tests/*.cog"))
test_commands = [["../cogni", file] for file in test_files]
test_processes = [subprocess.Popen(
    command, stderr=subprocess.STDOUT, stdout=subprocess.PIPE)
    for command in test_commands]
for f, p in zip(test_files, test_processes):
    test(f, p, max(map(len, test_files)))

print("Successes:", successes)
print(" Failures:", failures)
print("   Errors:", errors)
print("  Crashes:", crashes)
