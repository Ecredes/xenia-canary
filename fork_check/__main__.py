import sys
import traceback

try:
    # `python3 -m fork_check audit ...` runs the dependency/workflow audit.
    if sys.argv[1:2] == ["audit"]:
        del sys.argv[1]
        from fork_check.audit import main
    else:
        from fork_check.cli import main
except Exception:  # Import failures must exit 2 (broken), not 1 (findings).
    traceback.print_exc()
    sys.exit(2)

sys.exit(main())
