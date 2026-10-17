#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Publish a Build run's packages as releases, and say so on Discord.

    scripts/publish-builds.py plan --repo OWNER/REPO --to OWNER/NIGHTLY
    scripts/publish-builds.py publish --repo OWNER/REPO --to OWNER/NIGHTLY --kind nightly|pr|release --run ID
        --sha SHA [--pr N] [--tag TAG] --artifacts DIR [--checkout DIR] [--keep N]

plan follows the event GitHub Actions ran it for (GITHUB_EVENT_NAME, GITHUB_EVENT_PATH), and writes to GITHUB_OUTPUT
the run whose artifacts publish takes (kind, run, sha, short, pr, tag), when there is one to publish:
- a Build run that ended (workflow_run): a push of a version tag (v...) that built everywhere becomes REPO's release of
  that tag, the latest (a prerelease for a tag with a '-', v1.0.0-rc1); a push to main that built everywhere becomes
  NIGHTLY's release nightly-<date>-<commit>, the latest there, only the newest N nightlies staying, and REPO's
  prerelease "nightly" a link to it; a pull request's run becomes NIGHTLY's prerelease pr-<number>, its files and notes
  replaced at each push;
- a pull request closed (pull_request_target): its prerelease and tag are deleted;
- a fork's pull request is published only once someone of the team (write, maintain or admin) labels it LABEL after
  its last push, which a new push takes off (synchronize). The label put on a finished run publishes it at once.
Nothing is built: the files come from the run's artifacts, which the workflow downloads into DIR between the two (one
folder each, as actions/download-artifact puts them), and no code of the pull request runs. Each file is named
ScarletNX-<system>-<arch> (scripts/nightly-notes.py), an AppImage with the .zsync AppImageUpdate reads beside it.

Discord: each release and each nightly gets a message on DISCORD_WEBHOOK_RELEASES's and DISCORD_WEBHOOK_NIGHTLY's
channels, each pull request one message on DISCORD_WEBHOOK_PRS's, edited at each push and when it closes (its id is
kept in the prerelease's notes). A message links each download with its size, and says what changed: a nightly's or a
release's changes, a pull request's commits. Without a webhook nothing is posted, and the run says so. gh reads and writes REPO
with GH_TOKEN, and NIGHTLY with NIGHTLY_TOKEN. --dry-run reads and prints each write instead.
.github/workflows/publish.yml runs this; tests/publish_builds_test.py runs it against a fake gh and a fake Discord.
"""

import argparse
import datetime
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('nightly_notes', Path(__file__).with_name('nightly-notes.py'))
nightly_notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nightly_notes)

SERVER = nightly_notes.SERVER
LABEL = 'publish builds'
TEAM = {'admin', 'maintain', 'write'}
DISCORD = re.compile(r'<!-- discord-message: (\d+) -->')
VERSION = re.compile(r'v\d[0-9A-Za-z.+-]*')
ROLLING = 'nightly'
# A run's artifacts, by the commit's first 7 digits: the release file each gives, ScarletNX-<system>-<arch>
ARTIFACTS = {
    'ScarletNX-linux-x86_64-{short}.AppImage': 'ScarletNX-linux-x86_64.AppImage',
    'ScarletNX-linux-aarch64-{short}.AppImage': 'ScarletNX-linux-aarch64.AppImage',
    'ScarletNX-windows-x86_64-{short}': 'ScarletNX-windows-x86_64.zip',
    'ScarletNX-android-universal-{short}.apk': 'ScarletNX-android-universal.apk',
}
# the messages' colours: built, a pull request's build that missed a system, merged, closed
SCARLET, AMBER, MERGED, CLOSED = 0xB0102A, 0xD29922, 0x2EA043, 0x6E7681
# the mascot beside each message
WIMSY = 'https://raw.githubusercontent.com/{repo}/{branch}/dist/icons/hicolor/128x128/apps/wimsy.png'
# what a message lists, at most, before "and N more": a nightly's or a release's changes, a pull request's commits
MOST_LINES, MOST_COMMITS = 10, 6
# Discord's limits: an embed's description, and its whole text
DESCRIPTION, EMBED = 4096, 6000


class Publisher:
    def __init__(self, args):
        self.repo, self.to, self.dry = args.repo, args.to, args.dry_run
        self.checkout, self.keep = args.checkout, args.keep
        self.artifacts = Path(args.artifacts) if args.artifacts else None
        self.tokens = {self.repo: os.environ.get('GH_TOKEN', ''), self.to: os.environ.get('NIGHTLY_TOKEN', '')}
        self.work = Path(tempfile.mkdtemp(prefix='publish-builds-'))
        self.output = {}

    # ---- GitHub, through gh: REPO with GH_TOKEN, the nightly repository with NIGHTLY_TOKEN
    def token(self, repo):
        if repo == self.to and not self.tokens[repo] and not self.dry:
            raise RuntimeError(f'NIGHTLY_TOKEN is not set: nothing can be written in {self.to}')
        return self.tokens[repo]

    def gh(self, args, repo, data=None, write=False):
        if write and self.dry:
            print('would run: gh', ' '.join(args), json.dumps(data) if data is not None else '')
            return {}
        run = subprocess.run(
            ['gh', *args],
            input=json.dumps(data) if data is not None else None,
            capture_output=True,
            text=True,
            env=dict(os.environ, GH_TOKEN=self.token(repo)),
            check=False,
        )
        if run.returncode:
            raise RuntimeError(f'gh {" ".join(args)}: {run.stderr.strip() or run.stdout.strip()}')
        return json.loads(run.stdout) if run.stdout.strip() else {}

    def read(self, path):
        return self.gh(['api', path], self.repo)

    def pages(self, path, repo):
        """every item of a listing, 100 a page"""
        items, page = [], 1
        while True:
            sep = '&' if '?' in path else '?'
            got = self.gh(['api', f'{path}{sep}per_page=100&page={page}'], repo)
            got = got.get('artifacts', got.get('workflow_runs', got)) if isinstance(got, dict) else got
            items += got
            if len(got) < 100:
                return items
            page += 1

    def write(self, repo, method, path, data=None):
        args = ['api', '-X', method, path, *(['--input', '-'] if data is not None else [])]
        return self.gh(args, repo, data, write=True)

    # ---- releases, of REPO or of the nightly repository
    def releases(self, repo):
        return self.pages(f'repos/{repo}/releases', repo)

    def release(self, repo, tag):
        return next((r for r in self.releases(repo) if r['tag_name'] == tag), None)

    def put_release(self, repo, tag, name, body, prerelease, files, target=None):
        """the release of tag with these notes and exactly these files (each name to its path), made or replaced; it
        and whether it is new"""
        existing = self.release(repo, tag)
        fields = {'name': name, 'body': body, 'prerelease': prerelease, 'make_latest': str(not prerelease).lower()}
        if existing:
            made = self.write(repo, 'PATCH', f'repos/{repo}/releases/{existing["id"]}', fields) or existing
        else:
            fields = dict(fields, tag_name=tag, **({'target_commitish': target} if target else {}))
            made = self.write(repo, 'POST', f'repos/{repo}/releases', fields)
        if files:
            self.gh(
                ['release', 'upload', tag, *map(str, files.values()), '--repo', repo, '--clobber'], repo, write=True
            )
        for asset in (existing or {}).get('assets', []):
            if asset['name'] not in files:  # a file this run did not build: the old one would pass for it
                self.write(repo, 'DELETE', f'repos/{repo}/releases/assets/{asset["id"]}')
        return made, existing is None

    def drop_release(self, repo, release):
        self.write(repo, 'DELETE', f'repos/{repo}/releases/{release["id"]}')
        try:
            self.write(repo, 'DELETE', f'repos/{repo}/git/refs/tags/{release["tag_name"]}')
        except RuntimeError as error:
            if 'Reference does not exist' not in str(error):
                raise
            print(f'{release["tag_name"]} had no tag left')

    # ---- a run's packages
    def packages(self, sha):
        """the release files the run's artifacts give, by name, the files to upload with them (an AppImage's .zsync),
        and the release files it gave none for"""
        files, uploads, missing = {}, {}, []
        for pattern, name in ARTIFACTS.items():
            folder = self.artifacts / pattern.format(short=sha[:7])
            if not folder.is_dir() or not any(folder.iterdir()):
                missing.append(name)
                continue
            path = self.work / name
            if name.endswith('.zip'):  # the Windows build is a folder: zipped as it is
                with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
                    for item in sorted(folder.rglob('*')):
                        if item.is_file():
                            archive.write(item, item.relative_to(folder).as_posix())
            else:
                found = sorted(folder.rglob(name if name.endswith('.AppImage') else '*.apk'))
                if len(found) != 1:
                    raise RuntimeError(f'{folder.name} holds {len(found)} of {name}, not one')
                shutil.copyfile(found[0], path)
            files[name] = uploads[name] = path
            if name.endswith('.AppImage'):
                zsync = folder / f'{name}.zsync'
                if not zsync.is_file():
                    raise RuntimeError(f'{folder.name} has no {zsync.name}, which AppImageUpdate reads')
                uploads[zsync.name] = self.work / zsync.name
                shutil.copyfile(zsync, uploads[zsync.name])
        return files, uploads, missing

    def complete(self, run, sha):
        files, uploads, missing = self.packages(sha)
        if missing:
            said = '; '.join(map(nightly_notes.system, missing))
            raise RuntimeError(f'run {run} built everywhere, yet has no package for {said}')
        return files, uploads

    # ---- Discord
    def discord(self, kind, embed, message=None):
        """the message of kind (releases, nightly or prs) posted, or edited when message names it: its id, or None"""
        hook = os.environ.get(f'DISCORD_WEBHOOK_{kind.upper()}', '')
        if not hook:
            print(f'::notice::No DISCORD_WEBHOOK_{kind.upper()}: nothing posted on Discord')
            return message
        body = {'embeds': [embed], 'allowed_mentions': {'parse': []}}
        if self.dry:
            print(f'would {"edit" if message else "post"} on Discord ({kind}):', json.dumps(body))
            return message
        # a webhook of a forum channel or a thread names it in its query (thread_id=...), which both requests keep
        base, _, query = hook.partition('?')
        edit = f'{base}/messages/{message}' + (f'?{query}' if query else '')
        post = f'{base}?' + (f'{query}&' if query else '') + 'wait=true'
        for method, url in (('PATCH', edit), ('POST', post)):
            if method == 'PATCH' and not message:
                continue
            request = urllib.request.Request(
                url,
                data=json.dumps(body).encode(),
                method=method,
                # Discord refuses urllib's own agent
                headers={'Content-Type': 'application/json', 'User-Agent': f'DiscordBot ({SERVER}/{self.repo}, 1)'},
            )
            try:
                with urllib.request.urlopen(request, timeout=30) as reply:
                    return json.loads(reply.read() or b'{}').get('id', message)
            except urllib.error.HTTPError as error:
                if method == 'PATCH' and error.code == 404:  # deleted on Discord: a new one
                    continue
                print(f'::warning::Discord answered {error.code} to the {kind} message: {error.read()[:200]!r}')
                return message
            except (urllib.error.URLError, OSError) as error:
                print(f'::warning::Discord could not be reached for the {kind} message: {error}')
                return message
        return message

    @staticmethod
    def listed(lines, more_url, most=MOST_LINES):
        """the first most lines, the rest counted and linked"""
        shown = [f'- {line}' for line in lines[:most]]
        if len(lines) > most:
            shown.append(f'- and [{len(lines) - most} more]({more_url})')
        return shown

    def changes(self, previous, sha):
        """what changed since previous, as a message says it"""
        url = f'{SERVER}/{self.repo}'
        items = nightly_notes.changes(self.checkout, previous, sha)
        if not items:
            return f'Built from [`{sha[:7]}`]({url}/commit/{sha}).'
        compare = f'{url}/compare/{previous}...{sha}'
        lines = [nightly_notes.change_line(item, url) for item in items]
        return '\n'.join([f'**Changes** ([`{previous[:7]}...{sha[:7]}`]({compare}))', *self.listed(lines, compare)])

    @staticmethod
    def fields(files, sizes, download, missing=(), why=''):
        """a field for each system's download, with its size, in the order of the notes' table"""
        out = []
        for name in nightly_notes.ordered([*files, *missing]):
            system, arch, kind = nightly_notes.SYSTEMS[name]
            named = system if ',' in arch else f'{system} {arch}'  # one file for every architecture: the system says it
            if name in files:
                value = f'[{kind}]({download}/{name})\n{nightly_notes.size(sizes[name])}'
            else:
                value = f'Not built\n[see the run]({why})' if why else 'Not built'
            out.append({'name': named, 'value': value, 'inline': True})
        return out

    def embed(self, title, url, description, fields=(), color=SCARLET, footer='', author=None):
        """a message's embed, its description cut by whole lines, the last first, to what Discord takes: what it lists
        gives way, the downloads stay"""
        title = title[:256]
        room = EMBED - len(title) - len(footer) - len((author or {}).get('name', ''))
        room = max(0, min(DESCRIPTION, room - sum(len(f['name']) + len(f['value']) for f in fields)))
        while len(description) > room:
            description = description.rsplit('\n', 1)[0] if '\n' in description else description[: max(room, 0)]
        made = {
            'title': title,
            'url': url,
            'description': description,
            'color': color,
            'thumbnail': {'url': WIMSY.format(repo=self.repo, branch='main')},
            'timestamp': datetime.datetime.now(datetime.UTC).isoformat(timespec='seconds'),
        }
        if fields:
            made['fields'] = list(fields)
        if footer:
            made['footer'] = {'text': footer}
        if author:
            made['author'] = author
        return made

    def older(self, sha, previous):
        """sha is an ancestor of previous, a newer commit already published"""
        if not previous or previous == sha:
            return False
        command = ['git', '-C', self.checkout, 'merge-base', '--is-ancestor', sha, previous]
        return subprocess.run(command, capture_output=True, check=False).returncode == 0

    # ---- what each event publishes
    def nightly(self, run, sha):
        files, uploads = self.complete(run, sha)
        date = nightly_notes.git(self.checkout, 'log', '-1', '--format=%cs', sha).strip()
        tag = f'nightly-{date}-{sha[:7]}'
        older = sorted(
            (r for r in self.releases(self.to) if r['tag_name'].startswith('nightly-') and r['tag_name'] != tag),
            key=lambda r: r['created_at'],
            reverse=True,
        )
        previous = next((m.group(0) for r in older[:1] for m in [re.search(r'[0-9a-f]{40}', r['body'] or '')] if m), '')
        if self.older(sha, previous):  # a run that ended after a newer commit's: that one stays the latest
            print(f'{sha[:7]} is older than the newest nightly, {previous[:7]}: not published')
            return
        latest = f'{SERVER}/{self.to}/releases/latest'
        kept = (
            f'This repository keeps the newest {self.keep} nightlies; [the latest]({latest}) is always the newest one.'
        )
        download = f'{SERVER}/{self.to}/releases/download/{tag}'
        sizes = {name: path.stat().st_size for name, path in files.items()}
        body = nightly_notes.notes(self.checkout, sha, previous, self.repo, list(files), download, kept, sizes=sizes)
        title = nightly_notes.title(self.checkout, sha)
        made, new = self.put_release(self.to, tag, title, body, False, uploads)
        for old in older[self.keep - 1 :]:
            self.drop_release(self.to, old)
            print(f'deleted {old["tag_name"]}: past the newest {self.keep}')
        print(f'{"published" if new else "replaced the files of"} {tag}')
        url = made.get('html_url', f'{SERVER}/{self.to}/releases/tag/{tag}')
        lead = (
            f'The newest nightly, [{title}]({url}), from [{self.to}]({SERVER}/{self.to}/releases) where the newest '
            f'{self.keep} are kept. Built from main at'
        )
        rolling = nightly_notes.notes(
            self.checkout, sha, previous, self.repo, list(files), download, '', sizes=sizes, lead=lead
        )
        self.point_rolling(title, rolling, sha)
        if new:  # a run again of the same commit is no news
            commit = f'{SERVER}/{self.repo}/commit/{sha}'
            more = [
                {'name': 'Source', 'value': f'[`{sha[:7]}`]({commit})', 'inline': True},
                {'name': 'Every nightly', 'value': f'[{self.to}]({SERVER}/{self.to}/releases)', 'inline': True},
            ]
            self.discord(
                'nightly',
                self.embed(
                    title,
                    url,
                    self.changes(previous, sha),
                    self.fields(files, sizes, download) + more,
                    footer=f'The newest {self.keep} nightlies are kept',
                ),
            )

    def point_rolling(self, title, body, sha):
        """REPO's rolling prerelease ROLLING: no files, the latest nightly's notes, its tag on that commit"""
        rolling = self.release(self.repo, ROLLING)
        fields = {'name': f'{title}, in {self.to}', 'body': body, 'prerelease': True, 'make_latest': 'false'}
        if not rolling:
            self.write(
                self.repo, 'POST', f'repos/{self.repo}/releases', dict(fields, tag_name=ROLLING, target_commitish=sha)
            )
        else:
            self.write(self.repo, 'PATCH', f'repos/{self.repo}/git/refs/tags/{ROLLING}', {'sha': sha, 'force': True})
            self.write(self.repo, 'PATCH', f'repos/{self.repo}/releases/{rolling["id"]}', fields)
            for asset in rolling.get('assets', []):
                self.write(self.repo, 'DELETE', f'repos/{self.repo}/releases/assets/{asset["id"]}')
        print(f'{self.repo} "{ROLLING}" links {title}')

    def version(self, run, sha, tag):
        files, uploads = self.complete(run, sha)
        earlier = sorted(
            (
                r
                for r in self.releases(self.repo)
                if VERSION.fullmatch(r['tag_name']) and r['tag_name'] != tag and not r.get('draft')
            ),
            key=lambda r: r['created_at'],
            reverse=True,
        )
        since = earlier[0]['tag_name'] if earlier else ''
        previous = nightly_notes.git(self.checkout, 'rev-list', '-n', '1', since).strip() if since else ''
        download = f'{SERVER}/{self.repo}/releases/download/{tag}'
        sizes = {name: path.stat().st_size for name, path in files.items()}
        body = nightly_notes.notes(
            self.checkout,
            sha,
            previous,
            self.repo,
            list(files),
            download,
            '',
            since,
            sizes,
            f'ScarletNX {tag}, built from',
            'the newest version',
        )
        title = f'ScarletNX {tag}'
        made, new = self.put_release(self.repo, tag, title, body, '-' in tag, uploads, sha)
        print(f'{"published" if new else "replaced the files of"} {self.repo} {tag}')
        if new:
            url = made.get('html_url', f'{SERVER}/{self.repo}/releases/tag/{tag}')
            commit = f'{SERVER}/{self.repo}/commit/{sha}'
            more = [{'name': 'Source', 'value': f'[`{sha[:7]}`]({commit})', 'inline': True}]
            if since:
                compare = f'{SERVER}/{self.repo}/compare/{since}...{tag}'
                more.append({'name': 'Since', 'value': f'[{since}]({compare})', 'inline': True})
            self.discord(
                'releases',
                self.embed(title, url, self.changes(previous, sha), self.fields(files, sizes, download) + more),
            )

    def pull_facts(self, pr, sha, run):
        """what a pull request's notes and message say of it besides the files: its link, its commit's, the main commit
        it stands on and the nightly of that commit when there is one, to set the two side by side, its commits as lines
        and as subjects alone, for a message (the newest first, as a nightly's changes), the run's link"""
        url = f'{SERVER}/{self.repo}'
        number = pr['number']
        base = self.read(f'repos/{self.repo}/compare/{pr["base"]["sha"]}...{sha}')['merge_base_commit']['sha']
        commits = self.pages(f'repos/{self.repo}/pulls/{number}/commits', self.repo)
        subjects = [
            nightly_notes.escape(c['commit']['message'].splitlines()[0] if c['commit']['message'] else '')
            for c in reversed(commits)
        ]
        lines = [
            f'{subject} ([`{c["sha"][:7]}`]({url}/pull/{number}/commits/{c["sha"]}))'
            for subject, c in zip(subjects, reversed(commits))
        ]
        nightly = next(
            (
                r['html_url']
                for r in self.releases(self.to)
                if r['tag_name'].startswith('nightly-') and r['tag_name'].endswith(f'-{base[:7]}')
            ),
            '',
        )
        return {
            'pr': pr['html_url'],
            'commit': f'{url}/pull/{number}/commits/{sha}',
            'base': base,
            'base_said': f'[`{base[:7]}`]({url}/commit/{base})' + (f' ([its nightly]({nightly}))' if nightly else ''),
            'commits': lines,
            'subjects': subjects,
            'all_commits': f'{pr["html_url"]}/commits',
            'run': f'{url}/actions/runs/{run}',
            'author': pr['user']['login'],
            'author_url': pr['user'].get('html_url', f'{SERVER}/{pr["user"]["login"]}'),
        }

    def pull_notes(self, pr, sha, files, sizes, missing, facts, message):
        number = pr['number']
        download = f'{SERVER}/{self.to}/releases/download/pr-{number}'
        lines = [
            (
                f'Test builds of [#{number}]({facts["pr"]}) by [@{facts["author"]}]({facts["author_url"]}), at '
                f'[`{sha[:7]}`]({facts["commit"]}), on main at {facts["base_said"]}.'
            ),
            '',
            "A pull request's builds, not a release: each push to it replaces them, and they are deleted when it closes.",
            '',
            *nightly_notes.downloads(
                list(files),
                download,
                f'{SERVER}/{self.repo}',
                sha,
                sizes,
                missing,
                facts['run'],
                "the newest nightly, not this pull request's next builds",
            ),
        ]
        if facts['commits']:
            count = len(facts['commits'])
            lines += ['', '## Commits', '', f'{count} commit{"" if count == 1 else "s"}, the newest first:', '']
            lines += [f'- {line}' for line in facts['commits'][: nightly_notes.MOST_CHANGES]]
            if count > nightly_notes.MOST_CHANGES:
                lines.append(f'- and [{count - nightly_notes.MOST_CHANGES} more]({facts["all_commits"]})')
        lines += ['', *nightly_notes.source(facts['commit'])]
        if message:
            lines += ['', f'<!-- discord-message: {message} -->']
        return '\n'.join(lines) + '\n'

    def pull(self, pr, sha, run):
        number = pr['number']
        if pr['head']['sha'] != sha:
            print(f'#{number} is at {pr["head"]["sha"][:7]} now: the builds of {sha[:7]} are not published')
            return
        files, uploads, missing = self.packages(sha)
        if not files:
            print(f'#{number}: nothing of {sha[:7]} was built, so its builds stay as they were')
            return
        tag = f'pr-{number}'
        existing = self.release(self.to, tag)
        message = next(iter(DISCORD.findall((existing or {}).get('body') or '')), None)
        title = f'PR #{number}: {pr["title"]}'
        sizes = {name: path.stat().st_size for name, path in files.items()}
        facts = self.pull_facts(pr, sha, run)
        body = self.pull_notes(pr, sha, files, sizes, missing, facts, message)
        made, new = self.put_release(self.to, tag, title, body, True, uploads)
        print(f'{"published" if new else "replaced"} {tag} with the builds of {sha[:7]}')
        download = f'{SERVER}/{self.to}/releases/download/{tag}'
        said = (
            f'Test builds of [#{number}]({facts["pr"]}) at [`{sha[:7]}`]({facts["commit"]}), on main at '
            f'{facts["base_said"]}.'
        )
        if facts['commits']:
            count = len(facts['commits'])
            said += f'\n\n**{count} commit{"" if count == 1 else "s"}**, the newest first\n'
            said += '\n'.join(self.listed(facts['subjects'], facts['all_commits'], MOST_COMMITS))
        more = [
            {'name': 'Pull request', 'value': f'[#{number}]({facts["pr"]})', 'inline': True},
            {'name': 'Build', 'value': f'[Actions run]({facts["run"]})', 'inline': True},
        ]
        author = {'name': facts['author'], 'url': facts['author_url']}
        if pr['user'].get('avatar_url'):
            author['icon_url'] = pr['user']['avatar_url']
        embed = self.embed(
            title,
            made.get('html_url', f'{SERVER}/{self.to}/releases/tag/{tag}'),
            said,
            self.fields(files, sizes, download, missing, facts['run']) + more,
            AMBER if missing else SCARLET,
            'Replaced at each push, deleted when the pull request closes',
            author,
        )
        posted = self.discord('prs', embed, message)
        if posted and posted != message:  # kept for the next push to edit it
            body = self.pull_notes(pr, sha, files, sizes, missing, facts, posted)
            self.write(self.to, 'PATCH', f'repos/{self.to}/releases/{made.get("id")}', {'body': body})

    def closed(self, pr):
        tag = f'pr-{pr["number"]}'
        release = self.release(self.to, tag)
        if not release:
            print(f'#{pr["number"]} closed: it had no builds in {self.to}')
            return
        message = next(iter(DISCORD.findall(release.get('body') or '')), None)
        self.drop_release(self.to, release)
        print(f'deleted {tag}: #{pr["number"]} closed')
        if message:
            if pr.get('merged'):
                said = f'Merged into main: [the next nightly]({SERVER}/{self.to}/releases) has it. Its test builds are gone.'
            else:
                said = 'Closed without a merge: its test builds are gone.'
            embed = self.embed(
                f'PR #{pr["number"]}: {pr["title"]}', pr['html_url'], said, color=MERGED if pr.get('merged') else CLOSED
            )
            self.discord('prs', embed, message)

    # ---- who may publish a fork's pull request
    def fork(self, pr):
        return pr['head']['repo'] is None or pr['head']['repo']['full_name'] != self.repo

    def approved(self, pr, since):
        """labeled LABEL by the team after since (when the run of its head began), and still labeled"""
        if LABEL not in {label['name'] for label in pr.get('labels', [])}:
            return False
        events = self.pages(f'repos/{self.repo}/issues/{pr["number"]}/events', self.repo)
        for event in reversed(events):
            if event.get('event') != 'labeled' or event.get('label', {}).get('name') != LABEL:
                continue
            if event['created_at'] < since:
                return False
            login = event['actor']['login']
            try:
                permission = self.read(f'repos/{self.repo}/collaborators/{login}/permission').get('permission')
            except RuntimeError as error:
                permission = f'unknown: {error}'
            if permission in TEAM:
                return True
            print(f'"{LABEL}" was put by {login}, who is not of the team ({permission})')
        return False

    def pull_of(self, sha):
        """the open pull request whose head is sha"""
        pulls = self.pages(f'repos/{self.repo}/pulls?state=open', self.repo)
        return next((pr for pr in pulls if pr['head']['sha'] == sha), None)

    def take(self, kind, run, pr=None, tag=''):
        """the run publish takes the artifacts of, for the workflow to download them"""
        self.output = {
            'kind': kind,
            'run': str(run['id']),
            'sha': run['head_sha'],
            'short': run['head_sha'][:7],
            'pr': str(pr['number']) if pr else '',
            'tag': tag,
        }
        print(f'to publish: the {kind} builds of run {run["id"]} ({run["head_sha"][:7]})')

    def tagged(self, name):
        """name is a tag of REPO (a run's head_branch is the tag a tag push pushed)"""
        try:
            return bool(self.read(f'repos/{self.repo}/git/ref/tags/{urllib.parse.quote(name)}'))
        except RuntimeError:
            return False

    def on_run(self, run):
        if run['event'] == 'push':
            name = run['head_branch']
            if run['head_repository']['full_name'] != self.repo:
                print(f'a push to {run["head_repository"]["full_name"]}: nothing to publish')
            elif name != 'main' and not (VERSION.fullmatch(name) and self.tagged(name)):
                print(f'a push to {name}: neither main nor a version tag, nothing to publish')
            elif run['conclusion'] != 'success':
                print(f'run {run["id"]} of {name} ended {run["conclusion"]}: nothing published')
            elif name == 'main':
                self.take('nightly', run)
            else:
                self.take('release', run, tag=name)
            return
        if run['event'] != 'pull_request':
            print(f'a run of {run["event"]}: nothing to publish')
            return
        if run['conclusion'] == 'cancelled':
            print(f'run {run["id"]} was cancelled: a newer push has its own')
            return
        pr = self.pull_of(run['head_sha'])
        if not pr:
            print(f"{run['head_sha'][:7]} is no open pull request's head: nothing to publish")
        elif self.fork(pr) and not self.approved(pr, run['created_at']):
            print(f'#{pr["number"]} is a fork\'s: it waits for the team\'s "{LABEL}" label on this push')
        else:
            self.take('pr', run, pr)

    def on_pull(self, action, pr, label):
        if action == 'closed':
            self.closed(pr)
        elif action == 'synchronize' and self.fork(pr) and LABEL in {lb['name'] for lb in pr.get('labels', [])}:
            # what was let through was the push before: this one waits for the label again
            path = f'repos/{self.repo}/issues/{pr["number"]}/labels/{urllib.parse.quote(LABEL)}'
            self.gh(['api', '-X', 'DELETE', path], self.repo, write=True)
            print(f'took "{LABEL}" off #{pr["number"]}: a new push')
        elif action == 'labeled' and label == LABEL and self.fork(pr):
            path = f'repos/{self.repo}/actions/workflows/build.yml/runs?head_sha={pr["head"]["sha"]}'
            done = sorted(
                (r for r in self.pages(path, self.repo) if r['status'] == 'completed' and r['event'] == 'pull_request'),
                key=lambda r: r['created_at'],
            )
            if not done:
                print(f'#{pr["number"]}: its build has not ended; it is published when it does')
            elif self.approved(pr, done[-1]['created_at']):
                self.take('pr', done[-1], pr)
        else:
            print(f'pull request {action}: nothing to publish')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('step', choices=['plan', 'publish'])
    parser.add_argument('--repo', required=True)
    parser.add_argument('--to', required=True, help='the nightly repository')
    parser.add_argument('--kind', choices=['nightly', 'pr', 'release'])
    parser.add_argument('--run', help='the Build run the artifacts are of')
    parser.add_argument('--sha', help='the commit it built')
    parser.add_argument('--pr', type=int, help="the pull request's number")
    parser.add_argument('--tag', help="the release's version tag")
    parser.add_argument('--artifacts', help='the folder the workflow downloaded the artifacts into')
    parser.add_argument('--checkout', default='.', help="REPO's history and tags, for the notes")
    parser.add_argument('--keep', type=int, default=30, help='the nightlies kept')
    parser.add_argument('--dry-run', action='store_true', help='read, and print each write instead')
    args = parser.parse_args()
    if args.keep < 1:
        parser.error('--keep takes 1 or more')
    if args.step == 'publish' and not (args.kind and args.run and args.sha and args.artifacts):
        parser.error('publish takes --kind, --run, --sha and --artifacts')
    if args.kind == 'pr' and not args.pr:
        parser.error('--kind pr takes --pr')
    if args.kind == 'release' and not (args.tag and VERSION.fullmatch(args.tag)):
        parser.error('--kind release takes a version --tag (v...)')
    publisher = Publisher(args)
    try:
        if args.step == 'publish':
            if args.kind == 'nightly':
                publisher.nightly(args.run, args.sha)
            elif args.kind == 'release':
                publisher.version(args.run, args.sha, args.tag)
            else:
                publisher.pull(publisher.read(f'repos/{args.repo}/pulls/{args.pr}'), args.sha, args.run)
            return
        name = os.environ['GITHUB_EVENT_NAME']
        with open(os.environ['GITHUB_EVENT_PATH'], encoding='utf-8') as file:
            event = json.load(file)
        if name == 'workflow_run':
            publisher.on_run(event['workflow_run'])
        elif name == 'pull_request_target':
            publisher.on_pull(event['action'], event['pull_request'], event.get('label', {}).get('name'))
        else:
            print(f'{name}: nothing to publish')
        if publisher.output and os.environ.get('GITHUB_OUTPUT'):
            with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as file:
                file.writelines(f'{key}={value}\n' for key, value in publisher.output.items())
    finally:
        shutil.rmtree(publisher.work, ignore_errors=True)


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as error:
        sys.exit(f'publish-builds: {error}')
