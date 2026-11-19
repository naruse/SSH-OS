#!/usr/bin/env python3
"""A throw-away OpenSSH server for trying the SE's SSH client on this Mac.

    ssh_fixture.py DIRECTORY PORT [shell] [ask]

makes DIRECTORY/sshd with a new host key, a new user key that alone may log
in (as you), and an sshd_config for /usr/sbin/sshd to run without special
rights on 127.0.0.1 PORT. It also writes DIRECTORY/config-ssh.txt, settings
for an SSH OS boot disk that reaches that server from the Snow emulator.
With "shell" a login gets your own shell; otherwise /bin/sh with the prompt
"ready> ", which tests can wait for. With "ask" the settings leave out the
server's key, so the first connection asks whether to trust it.

Nothing here starts or stops the server. Start it with
    /usr/sbin/sshd -D -E DIRECTORY/sshd.log -f DIRECTORY/sshd/sshd_config
"""
import getpass
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEYS = ("hostkey", "userkey", "otherkey")


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def key_setting(keyfile):
    """The ssh_key line for a private key file."""
    return run(sys.executable, os.path.join(ROOT, "tools", "sshkey.py"), keyfile).strip()


def prepare(directory, port, own_shell=False, extra=()):
    """Returns the settings lines: (ssh_hosts, ssh_key, an ssh_key the server does not know)."""
    directory = os.path.abspath(directory)
    os.makedirs(directory, exist_ok=True)
    for name in KEYS:
        for old in (name, name + ".pub"):           # ssh-keygen will not overwrite
            if os.path.exists(os.path.join(directory, old)):
                os.remove(os.path.join(directory, old))
        run("ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "sshos-test", "-f",
            os.path.join(directory, name))
    shutil.copy(os.path.join(directory, "userkey.pub"), os.path.join(directory, "authorized_keys"))
    lines = [
        "Port %d" % port, "ListenAddress 127.0.0.1",
        # quoted: the project folder's name may have a space in it
        'HostKey "%s"' % os.path.join(directory, "hostkey"),
        'PidFile "%s"' % os.path.join(directory, "sshd.pid"),
        'AuthorizedKeysFile "%s"' % os.path.join(directory, "authorized_keys"),
        "StrictModes no", "UsePAM no", "PubkeyAuthentication yes",
        "PasswordAuthentication yes", "KbdInteractiveAuthentication no",
        "AcceptEnv LANG LC_*", "LogLevel DEBUG3",
    ] + list(extra)
    if not own_shell:
        lines.append("ForceCommand PS1='ready> ' /bin/sh")
    with open(os.path.join(directory, "sshd_config"), "w") as f:
        f.write("\n".join(lines) + "\n")
    # the fingerprint as OpenSSH itself prints it
    fingerprint = run("ssh-keygen", "-lf", os.path.join(directory, "hostkey.pub")).split()[1]
    return ("ssh_hosts = 127.0.0.1 " + fingerprint,
            key_setting(os.path.join(directory, "userkey")),
            key_setting(os.path.join(directory, "otherkey")))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip())
    directory, port = sys.argv[1], int(sys.argv[2])
    own_shell = "shell" in sys.argv[3:]
    os.makedirs(directory, exist_ok=True)
    known, key, _ = prepare(os.path.join(directory, "sshd"), port, own_shell)
    with open(os.path.join(directory, "config-ssh.txt"), "w") as f:
        f.write("\n".join([
            "# Made by tests/ssh_fixture.py: reach the test server on this Mac from Snow.",
            "network = 10.0.0.2 255.0.0.0 10.0.0.1 1.1.1.1",
            "ssh = %s@127.0.0.1:%d" % (getpass.getuser(), port),
            key, "" if "ask" in sys.argv[3:] else known,
            # with your own shell (the demo) the disk starts in the home screen
            "" if own_shell else "termlog = 1\nstart = shell", ""]))


if __name__ == "__main__":
    main()
