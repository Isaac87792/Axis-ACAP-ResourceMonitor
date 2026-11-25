#!/usr/bin/env python3
"""Unpack the Axis ACAP Native SDK image into a folder, without Docker.

Usage: fetch_sdk.py <image> <tag> <dest>
Downloads the linux/amd64 image from Docker Hub and applies its layers in
order (including whiteout files), so <dest> can be used as a chroot.
"""
import json
import os
import shutil
import sys
import tarfile
import urllib.request

REGISTRY = "https://registry-1.docker.io"
ACCEPT = ", ".join([
    "application/vnd.oci.image.index.v1+json",
    "application/vnd.docker.distribution.manifest.list.v2+json",
    "application/vnd.oci.image.manifest.v1+json",
    "application/vnd.docker.distribution.manifest.v2+json",
])


def get(url, token, accept=None):
    req = urllib.request.Request(url, headers={"Authorization": "Bearer " + token})
    if accept:
        req.add_header("Accept", accept)
    return urllib.request.urlopen(req, timeout=120)


def remove(path):
    if os.path.islink(path) or os.path.isfile(path):
        os.unlink(path)
    elif os.path.isdir(path):
        shutil.rmtree(path)


def apply_layer(stream, dest):
    with tarfile.open(fileobj=stream, mode="r|*") as tar:
        for member in tar:
            name = os.path.normpath(member.name).lstrip("/")
            if name.startswith(".."):
                continue
            base = os.path.basename(name)
            folder = os.path.join(dest, os.path.dirname(name))
            if base == ".wh..wh..opq":
                if os.path.isdir(folder):
                    for entry in os.listdir(folder):
                        remove(os.path.join(folder, entry))
                continue
            if base.startswith(".wh."):
                remove(os.path.join(folder, base[4:]))
                continue
            target = os.path.join(dest, name)
            if os.path.lexists(target) and not (member.isdir() and os.path.isdir(target)):
                remove(target)
            tar.extract(member, dest, set_attrs=True, numeric_owner=True, filter="fully_trusted")


def main():
    image, tag, dest = sys.argv[1:4]
    token = json.load(urllib.request.urlopen(
        "https://auth.docker.io/token?service=registry.docker.io&scope=repository:%s:pull" % image,
        timeout=60))["token"]
    manifest = json.load(get("%s/v2/%s/manifests/%s" % (REGISTRY, image, tag), token, ACCEPT))
    if "manifests" in manifest:
        pick = [m for m in manifest["manifests"]
                if m.get("platform", {}).get("os") == "linux"
                and m.get("platform", {}).get("architecture") == "amd64"]
        if not pick:
            sys.exit("no linux/amd64 image for %s:%s" % (image, tag))
        manifest = json.load(get("%s/v2/%s/manifests/%s" % (REGISTRY, image, pick[0]["digest"]), token, ACCEPT))

    config = json.load(get("%s/v2/%s/blobs/%s" % (REGISTRY, image, manifest["config"]["digest"]), token))
    os.makedirs(dest, exist_ok=True)
    layers = manifest["layers"]
    for i, layer in enumerate(layers, 1):
        print("layer %d/%d %s (%d MB)" % (i, len(layers), layer["digest"][:19], layer.get("size", 0) // 1048576), flush=True)
        apply_layer(get("%s/v2/%s/blobs/%s" % (REGISTRY, image, layer["digest"]), token), dest)

    with open(os.path.join(dest, ".image-env"), "w") as f:
        for line in config.get("config", {}).get("Env", []) or []:
            f.write("export %s\n" % shlex_quote(line))


def shlex_quote(env_line):
    key, _, value = env_line.partition("=")
    return "%s='%s'" % (key, value.replace("'", "'\\''"))


if __name__ == "__main__":
    main()
