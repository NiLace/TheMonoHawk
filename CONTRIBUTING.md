# Contributing

Contributions are welcome: bug reports, fixes, measurements, improvements.

## The licence of what you contribute

This project is licensed under the **GNU Affero General Public License v3.0, that version only**
(`AGPL-3.0-only`). By submitting a contribution you agree that it is licensed under exactly the same
terms (*inbound = outbound*).

There is **no CLA** and no copyright assignment: you keep the copyright of what you write. That is
deliberate — with the rights spread among everyone who contributed, nobody, the maintainer included,
can ever take the project closed.

Do not submit code copied from other projects unless its licence allows it to be combined under
AGPL-3.0-only, and say where it comes from.

## Sign your commits (DCO)

Commit with `git commit -s`. It adds a `Signed-off-by: Name <email>` line, by which you certify the
[Developer Certificate of Origin 1.1](https://developercertificate.org/): you wrote the change, or
otherwise have the right to submit it under this licence. A pseudonym is fine.

## New source files

Start each new source file with:

```c
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
```

(`#` instead of `//` in scripts, Makefiles and build files.) Generated files carry no header: they
are covered by the repository's `LICENSE` like everything else.
