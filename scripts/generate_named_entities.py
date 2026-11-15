#!/usr/bin/env python3
"""Generate/check the terminal entity table from Python's HTML5 dataset.

The committed table was generated with CPython 3.14.4. Python's html.entities
is distributed under the Python Software Foundation License; see
https://docs.python.org/3/license.html. Review changes when using a different
Python release. This script needs only the Python standard library.
"""
import argparse
import html.entities
from pathlib import Path
import sys


def generate():
    entries = sorted((key[:-1], value) for key, value in html.entities.html5.items()
                     if key.endswith(';'))
    lines = [
        '// HTML5 named character references from Python html.entities.html5.\n',
        '// Names omit their required semicolon; values are UTF-8.\n',
        '#pragma once\n#include <array>\n#include <string_view>\n#include <utility>\n',
        'namespace octopus::cli_detail {\n',
        'inline constexpr std::array<std::pair<std::string_view, std::string_view>, '
        f'{len(entries)}> kNamedEntities{{{{\n',
    ]
    for name, value in entries:
        encoded = ''.join(f'\\x{byte:02x}' for byte in value.encode('utf-8'))
        lines.append(f'    {{"{name}", "{encoded}"}},\n')
    lines.append('}};\n}\n')
    return ''.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='verify without writing')
    args = parser.parse_args()
    path = Path(__file__).resolve().parents[1] / 'src/cli/terminal/named_entities.hpp'
    result = generate()
    if args.check:
        if path.read_text(encoding='utf-8') != result:
            print(f'{path}: does not match Python {sys.version.split()[0]} data', file=sys.stderr)
            return 1
    else:
        path.write_text(result, encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
