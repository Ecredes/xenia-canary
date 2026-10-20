import sys
import traceback

try:
    from integrate.cli import main
except Exception:  # Import failures must exit 2 (broken), not 1 (conflict).
    traceback.print_exc()
    sys.exit(2)

sys.exit(main())
