#!/usr/bin/env python3
"""Validate the Windows Editor suite with the native acceptance contract."""
import argparse
import importlib.util
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-Project', type=Path, required=True)
    parser.add_argument('-Report', type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / 'build' / 'run_acceptance.py'
    spec = importlib.util.spec_from_file_location('qinglan_acceptance', source)
    acceptance = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(acceptance)
    expected, _ = acceptance.discover_tests(args.Project)
    result = acceptance.validate_report(args.Report, expected, campaign_enabled=False)
    print(f"Windows regression report: {result['named_successes']} named successes")


if __name__ == '__main__':
    main()
