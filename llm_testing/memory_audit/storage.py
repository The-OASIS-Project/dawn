# Memory audit: where its files live, and how they are written.
#
# Everything the audit reads or writes holds a user's facts or conversations,
# so it stays under data/ (ignored by git; the directory 0700) and every file
# is created 0600 before anything is written to it.
#
# License: GPLv3, same as DAWN.

import hashlib
import os

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")


def data_path(name):
    """A path under data/: a bare name goes there, any other path must already
    be inside it (so private files never land in the working tree)."""
    os.makedirs(DATA, mode=0o700, exist_ok=True)
    os.chmod(DATA, 0o700)
    path = os.path.realpath(name if os.path.dirname(name) else os.path.join(DATA, name))
    if os.path.commonpath([path, os.path.realpath(DATA)]) != os.path.realpath(DATA):
        raise SystemExit(f"{name}: audit files stay under {DATA}")
    return path


def private_open(path, mode="w"):
    """Open for writing ("w") or appending ("a") as a 0600 file, permissions
    set before the first byte is written."""
    flags = os.O_WRONLY | os.O_CREAT | (os.O_APPEND if mode == "a" else os.O_TRUNC)
    fd = os.open(path, flags, 0o600)
    os.fchmod(fd, 0o600)
    return os.fdopen(fd, mode)


def text_sha(text):
    """Fingerprint of a fact's text, so a classification is only ever joined to
    the fact it was made for (scratch databases reuse fact ids)."""
    return hashlib.sha1((text or "").encode()).hexdigest()[:16]
