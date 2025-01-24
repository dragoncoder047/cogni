import glob
import subprocess

test_files = sorted(glob.glob("cognac/tests/*.cog"))

successes = 0
failures = 0
errors = 0
crashes = 0


def test(file):
    global successes
    global failures
    global errors
    global crashes
    print("Testing: ", file, end="... ")
    try:
        out = subprocess.check_output(
            ["./cogni", file], stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as e:
        crashes += 1
        print("CRASH", end=" ")
        out = e.output
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


for file in test_files:
    test(file)

print("Successes:", successes)
print(" Failures:", failures)
print("   Errors:", errors)
print("  Crashes:", crashes)
