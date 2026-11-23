#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Usage: fetch-jam-downloads.py <download-dir>
# Fetches every file that build/jam/packages/* downloads with DownloadFile into
# <download-dir> and checks it against its pinned SHA-256. The arm64 build runs
# jam with --no-downloads, which stops these downloads too, so a firmware
# package added later would otherwise break that build.
#
# It reads the rules as jam tokens: `local name = value ;` with a single value,
# and `[ DownloadFile <file> : <url> : <...> : <sha256> ]`, where $(name) in the
# URL is expanded from those locals. A rule it cannot read is an error.
import hashlib
import os
import re
import sys
import time
import urllib.request

if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
	sys.exit("usage: fetch-jam-downloads.py <download-dir>")
dest = sys.argv[1]
packages = os.path.join(os.path.dirname(__file__), "..", "..", "build", "jam", "packages")

def tokens(path):
	text = open(path, encoding="utf-8").read()
	text = re.sub(r"(?m)#.*$", "", text)
	return text.split()

def expand(token, local, path, depth=0):
	def sub(match):
		if match.group(1) not in local or depth > 10:
			sys.exit(f"{path}: cannot expand $({match.group(1)})")
		return expand(local[match.group(1)], local, path, depth + 1)
	return re.sub(r"\$\(([A-Za-z0-9_]+)\)", sub, token)

def fetch(url, target):
	for attempt in range(5):
		try:
			with urllib.request.urlopen(url, timeout=60) as response:
				data = response.read()
			with open(target, "wb") as f:
				f.write(data)
			return
		except OSError as error:
			print(f"  {url}: {error}, retrying", file=sys.stderr)
			time.sleep(5 * (attempt + 1))
	sys.exit(f"could not download {url}")

found = 0
for name in sorted(os.listdir(packages)):
	path = os.path.join(packages, name)
	t = tokens(path)
	local = {}
	for i, token in enumerate(t):
		if token == "local" and i + 3 < len(t) and t[i + 2] == "=" and t[i + 4:i + 5] == [";"]:
			local[t[i + 1]] = t[i + 3]
		if token != "DownloadFile":
			continue
		# DownloadFile <file> : <url> : [<flags>] : <sha256> ]
		args = t[i + 1:t.index("]", i)]
		fields = [[]]
		for a in args:
			if a == ":":
				fields.append([])
			else:
				fields[-1].append(a)
		if len(fields) < 4 or len(fields[0]) != 1 or len(fields[1]) != 1 \
				or len(fields[3]) != 1 or not re.fullmatch(r"[0-9a-f]{64}", fields[3][0]):
			sys.exit(f"{path}: cannot read DownloadFile {' '.join(args)}")
		file, url, sha = fields[0][0], expand(fields[1][0], local, path), fields[3][0]
		target = os.path.join(dest, file)
		fetch(url, target)
		actual = hashlib.sha256(open(target, "rb").read()).hexdigest()
		if actual != sha:
			os.remove(target)
			sys.exit(f"{file}: SHA-256 {actual}, expected {sha} ({path})")
		print(f"{file}: OK ({name})")
		found += 1

if not found:
	sys.exit(f"no DownloadFile found in {packages}")
