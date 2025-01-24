import glob
import subprocess

successes = 0
failures = 0
errors = 0
crashes = 0

def test(file, process):
    global successes
    global failures
    global errors
    global crashes
    print("Testing: ", file, end="... ")
    exit_code = process.wait()
    out = process.stdout.read()
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
    print()

test_files = sorted(glob.glob("cognac/tests/*.cog"))
test_commands = ["./cogni " + file for file in test_files]
test_processes = [subprocess.Popen(command, shell=True, stderr=subprocess.PIPE, stdout=subprocess.PIPE) for command in test_commands]
for p in zip(test_files, test_processes):
    test(p[0], p[1])

print("Successes:", successes)
print(" Failures:", failures)
print("   Errors:", errors)
print("  Crashes:", crashes)
