import glob
import subprocess
import re

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
    exit_code = process.wait()
    out: bytes = process.stdout.read()
    if exit_code != 0:
        crashes += 1
        print("CRASH", end=" ")
    if b"ERROR" in out:
        errors += 1
        print("ERROR", end=" ")
    elif b"PASS" in out:
        successes += 1
        print("PASS", end=" ")
    else:
        failures += 1
        print("FAIL", end=" ")
    with open(file.replace(".cog", ".log"), "wb") as f:
        f.write(out)
    if b"assertion failed" in out.lower() or b"segfault" in out.lower():
        print(out.decode(), end="")
    elif (m := re.search(r"undefined: (.+?)\n", out.decode(), re.I)):
        print("requires", m.group(1), end="")
    print()


test_files = sorted(glob.glob("cognac/tests/*.cog"))
test_commands = ["./cogni " + file for file in test_files]
test_processes = [subprocess.Popen(
    command, shell=True, stderr=subprocess.STDOUT, stdout=subprocess.PIPE)
    for command in test_commands]
for f, p in zip(test_files, test_processes):
    test(f, p, max(map(len, test_files)))

print("Successes:", successes)
print(" Failures:", failures)
print("   Errors:", errors)
print("  Crashes:", crashes)
