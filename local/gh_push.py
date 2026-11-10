# Pushes a folder's files to a GitHub repository branch as one commit through the Git Data API (no git needed), with the gh CLI's token.
# Usage: python -I gh_push.py <gh.exe> <owner/repo> <branch> <local folder> <commit message> [--delete path ...]
import base64
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

gh, repo, branch, root, message = sys.argv[1:6]
token = subprocess.run([gh, 'auth', 'token'], capture_output=True, text=True, check=True).stdout.strip()


def api(method, path, body=None, ok404=False):
    for attempt in range(6):
        req = urllib.request.Request(f'https://api.github.com/repos/{repo}/{path}', method=method,
                                     data=None if body is None else json.dumps(body).encode(),
                                     headers={'Authorization': f'Bearer {token}', 'Accept': 'application/vnd.github+json',
                                              'X-GitHub-Api-Version': '2022-11-28', 'User-Agent': 'aai-gh-push'})
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                return json.loads(r.read() or b'null')
        except urllib.error.HTTPError as e:
            if e.code == 404 and ok404:
                return None
            if e.code < 500:
                raise SystemExit(f'{method} {path}: HTTP {e.code} {e.read()[:300]!r}')
        except (urllib.error.URLError, ConnectionError, TimeoutError) as e:
            print(f'  retry {method} {path}: {e}', flush=True)
        time.sleep(2 + 3 * attempt)
    raise SystemExit(f'{method} {path}: failed')


ref = api('GET', f'git/ref/heads/{branch}', ok404=True)
parent = ref['object']['sha'] if ref else None
base_tree = api('GET', f'git/commits/{parent}')['tree']['sha'] if parent else None
tree = []
for dp, dirs, files in os.walk(root):
    dirs[:] = sorted(d for d in dirs if d != '.git')
    for f in sorted(files):
        p = os.path.join(dp, f)
        rel = os.path.relpath(p, root).replace(os.sep, '/')
        blob = api('POST', 'git/blobs', {'content': base64.b64encode(open(p, 'rb').read()).decode(), 'encoding': 'base64'})
        mode = '100755' if rel.endswith(('.py', '.sh')) and rel.startswith(('ci/',)) else '100644'
        tree.append({'path': rel, 'mode': mode, 'type': 'blob', 'sha': blob['sha']})
        print(f'  {rel}', flush=True)
t = api('POST', 'git/trees', {'tree': tree, **({'base_tree': base_tree} if base_tree else {})})
c = api('POST', 'git/commits', {'message': message, 'tree': t['sha'], 'parents': [parent] if parent else []})
if ref:
    api('PATCH', f'git/refs/heads/{branch}', {'sha': c['sha']})
else:
    api('POST', 'git/refs', {'ref': f'refs/heads/{branch}', 'sha': c['sha']})
print(f'{repo}@{branch}: {c["sha"]}')
