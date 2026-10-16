import sys
import traceback

try:
    from fork_check.cli import main
except Exception:  # Import failures must exit 2 (broken), not 1 (findings).
    traceback.print_exc()
    sys.exit(2)

sys.exit(main())
