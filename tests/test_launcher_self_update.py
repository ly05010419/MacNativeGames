"""MNG Launcher replaces itself with a newer release (user 2026-11-18: a real update button, "点一下就自动下载、替换、重开";
the version label only linked to the disk image). tools/builder_app/launcher_update.py downloads the release's ZIP, takes
the app only when its signature satisfies the running Launcher's own requirement (and, for a Developer ID Launcher, when
macOS says it is notarized), and a shell script outside the app swaps the two once the Launcher has quit.

The whole run is made here with small apps signed ad hoc, a release list and a ZIP served from this Mac: the update
itself, and every refusal with the old app left as it was. With MNG_TEST_LAUNCHER_RELEASES=<folder holding
<version>/release/ of two signed releases> the same run is made with real Developer ID, notarized Launchers."""
import functools
import http.server
import importlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools/builder_app/launcher_update.py'
SWIFT = ROOT / 'tools/builder_app/Sources/MNGBuilder'
lu = importlib.import_module('tools.builder_app.launcher_update')
BUNDLE_ID = 'org.macnativegames.launcher'
APP = 'MNG Launcher.app'


def sign(app, identifier):
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', f'-r=designated => identifier "{identifier}"', str(app)],
                   check=True, capture_output=True)


def make_app(folder, version, program, identifier=BUNDLE_ID, name=APP):
    """A small app that says a version, signed ad hoc with the bundle ID as its designated requirement (make_app.py's way)."""
    app = Path(folder) / name
    (app / 'Contents/MacOS').mkdir(parents=True)
    (app / 'Contents/Resources').mkdir()
    shutil.copy2(program, app / 'Contents/MacOS/MNGBuilder')
    (app / 'Contents/Resources/version.txt').write_text(version + '\n')
    with open(app / 'Contents/Info.plist', 'wb') as f:
        plistlib.dump({'CFBundleIdentifier': identifier, 'CFBundleShortVersionString': version, 'CFBundleVersion': version,
                       'CFBundleExecutable': 'MNGBuilder', 'CFBundlePackageType': 'APPL'}, f)
    sign(app, identifier)
    return app


def zip_app(app, archive):
    subprocess.run(['/usr/bin/ditto', '-c', '-k', '--keepParent', str(app), str(archive)], check=True)
    return archive


def version_of(app):
    return plistlib.loads((Path(app) / 'Contents/Info.plist').read_bytes())['CFBundleShortVersionString']


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


class Served(unittest.TestCase):
    """A folder served on this Mac, a release list in it, and a place where "the running Launcher" is."""

    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler or not Path('/usr/bin/codesign').exists():
            raise unittest.SkipTest('A C compiler and codesign are required')
        cls.tools = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tools.cleanup)
        source = Path(cls.tools.name) / 'main.c'
        source.write_text('int main(void) { return 0; }\n')
        cls.program = Path(cls.tools.name) / 'main'
        subprocess.run([compiler, '-o', str(cls.program), str(source)], check=True, capture_output=True)

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        root = Path(self.folder.name).resolve()
        self.site, self.apps, self.support, self.made = root / 'site', root / 'Applications', root / 'support', root / 'made'
        for d in (self.site, self.apps, self.support, self.made):
            d.mkdir()
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Quiet, directory=str(self.site)))
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.base = f'http://127.0.0.1:{self.server.server_address[1]}'

    def publish(self, version, app=None, size=None, identifier=BUNDLE_ID, inner=None):
        """A release in the list: the ZIP of `app` (made here when not given; `inner`: the version the app itself says)."""
        if app is None:
            app = make_app(self.made / version, inner or version, self.program, identifier)
        name = f'MNG-Launcher-{version}-macOS-arm64.zip'
        archive = zip_app(app, self.site / name)
        release = {'tag_name': f'launcher-v{version}', 'draft': False, 'html_url': f'https://github.com/o/r/releases/tag/launcher-v{version}',
                   'assets': [{'name': name, 'size': archive.stat().st_size if size is None else size,
                               'browser_download_url': f'{self.base}/{name}'}]}
        (self.site / 'releases.json').write_text(json.dumps([release]))
        return release

    def run_update(self, app, *extra, wait=True):
        """The update as the window runs it, with a "Launcher" that quits a moment later; returns (exit code, events,
        the outcome the replacement wrote or None)."""
        launcher = subprocess.Popen(['/bin/sleep', '0.4'])
        env = dict(os.environ, MNG_LAUNCHER_UPDATE_NO_OPEN='1', MNG_LANG='en')
        done = subprocess.run([sys.executable, '-B', str(SCRIPT), 'update', '--app', str(app), '--support', str(self.support),
                               '--pid', str(launcher.pid), '--feed', f'{self.base}/releases.json', *extra],
                              capture_output=True, text=True, env=env, timeout=120)
        launcher.wait()
        events = [json.loads(line) for line in done.stdout.splitlines() if line.startswith('{')]
        result = self.support / 'launcher-update/result.json'
        outcome = None
        if wait and done.returncode == 0:
            for _ in range(200):
                if result.exists() and result.read_text().strip():
                    outcome = json.loads(result.read_text())
                    break
                time.sleep(0.05)
        return done.returncode, events, outcome

    def leftovers(self):
        return sorted(p.name for p in self.apps.iterdir() if p.name != APP) + sorted(
            p.name for p in (self.support / 'launcher-update').glob('*') if p.name not in ('.lock', 'replace.sh', 'replace.log', 'result.json'))


class Update(Served):
    def test_the_newer_release_takes_the_launchers_place(self):
        running = make_app(self.apps, '1.0.6', self.program)
        self.publish('1.0.7')
        code, events, outcome = self.run_update(running)
        self.assertEqual(code, 0, events)
        kinds = [e['event'] for e in events]
        self.assertEqual([k for i, k in enumerate(kinds) if i == 0 or kinds[i - 1] != k], ['found', 'download', 'verify', 'install'])
        self.assertEqual(events[0]['version'], '1.0.7')
        last = [e for e in events if e['event'] == 'download'][-1]
        self.assertEqual(last['done'], last['total'])
        self.assertEqual(outcome, {'ok': True})
        self.assertEqual(version_of(running), '1.0.7')
        self.assertEqual((running / 'Contents/Resources/version.txt').read_text(), '1.0.7\n')
        self.assertEqual(subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(running)]).returncode, 0)
        self.assertEqual(self.leftovers(), [])                       # no old app, no download, no unpacked copy
        self.assertIn('replaced', (self.support / 'launcher-update/replace.log').read_text())

    def test_the_same_version_only_for_tests(self):
        running = make_app(self.apps, '1.0.7', self.program)
        self.publish('1.0.7')
        code, events, _ = self.run_update(running)
        self.assertEqual((code, events[-1]['event']), (1, 'error'))
        self.assertIn('newest MNG Launcher (1.0.7)', events[-1]['message'])
        code, events, outcome = self.run_update(running, '--same')
        self.assertEqual((code, outcome), (0, {'ok': True}), events)

    def test_an_older_release_is_never_installed(self):
        running = make_app(self.apps, '1.0.7', self.program)
        self.publish('1.0.6')
        for extra in ((), ('--same',)):
            code, events, _ = self.run_update(running, *extra)
            self.assertEqual((code, events[-1]['event']), (1, 'error'), extra)
        self.assertEqual(version_of(running), '1.0.7')

    def refused(self, running, words):
        code, events, _ = self.run_update(running)
        self.assertEqual((code, events[-1]['event']), (1, 'error'), events)
        self.assertIn(words, events[-1]['message'])
        self.assertEqual(version_of(running), '1.0.6')               # the old Launcher is as it was
        self.assertEqual(subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(running)]).returncode, 0)
        self.assertEqual(self.leftovers(), [])                       # and nothing of the refused download is kept
        return events

    def test_another_app_is_refused(self):
        running = make_app(self.apps, '1.0.6', self.program)
        self.publish('1.0.7', identifier='org.example.other')
        self.refused(running, 'another app')

    def test_an_app_signed_otherwise_is_refused(self):
        running = make_app(self.apps, '1.0.6', self.program)
        other = make_app(self.made / 'x', '1.0.7', self.program)
        # its Info.plist says our bundle ID, its signature another identifier: not what the running Launcher requires
        subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--identifier', 'org.example.other',
                        '-r=designated => identifier "org.example.other"', str(other)], check=True, capture_output=True)
        self.publish('1.0.7', app=other)
        self.refused(running, 'not signed as this Launcher is')

    def test_a_changed_app_is_refused(self):
        running = make_app(self.apps, '1.0.6', self.program)
        changed = make_app(self.made / 'x', '1.0.7', self.program)
        (changed / 'Contents/Resources/version.txt').write_text('1.0.7 and something else\n')   # after it was signed
        self.publish('1.0.7', app=changed)
        self.refused(running, 'code signature is not whole')

    def test_an_app_of_another_version_than_the_release_is_refused(self):
        running = make_app(self.apps, '1.0.6', self.program)
        self.publish('1.0.8', inner='1.0.5')                         # an old signed release under a new name
        self.refused(running, "says version '1.0.5', the release 1.0.8")

    def test_a_download_of_another_size_is_refused(self):
        running = make_app(self.apps, '1.0.6', self.program)
        release = self.publish('1.0.7')
        release['assets'][0]['size'] += 1
        (self.site / 'releases.json').write_text(json.dumps([release]))
        self.refused(running, 'the release says')

    def test_an_archive_that_would_write_outside_the_app_is_not_unpacked(self):
        running = make_app(self.apps, '1.0.6', self.program)

        def outside(z):
            z.writestr('../outside.txt', 'x')

        def beside(z):
            z.writestr('beside.txt', 'x')

        def link(target):
            def add(z):
                entry = zipfile.ZipInfo(f'{APP}/Contents/Resources/link')
                entry.create_system, entry.external_attr = 3, (stat.S_IFLNK | 0o777) << 16
                z.writestr(entry, target)
            return add

        for change in (outside, beside, link('/etc'), link('../../../outside')):
            release = self.publish('1.0.7')
            archive = self.site / release['assets'][0]['name']
            with zipfile.ZipFile(archive, 'a') as z:
                change(z)
            release['assets'][0]['size'] = archive.stat().st_size
            (self.site / 'releases.json').write_text(json.dumps([release]))
            self.refused(running, 'not an archive of the app alone')
            self.assertEqual(sorted(p.name for p in self.support.iterdir()), ['launcher-update'])
            shutil.rmtree(self.made / '1.0.7')
        # a link that stays inside the app is what a signed app has (its Python's, its frameworks')
        good = make_app(self.made / 'good', '1.0.7', self.program)
        os.symlink('version.txt', good / 'Contents/Resources/current.txt')
        os.symlink('../Resources/version.txt', good / 'Contents/MacOS/up-and-in.txt')
        sign(good, BUNDLE_ID)
        self.publish('1.0.7', app=good)
        code, events, outcome = self.run_update(running)
        self.assertEqual((code, outcome), (0, {'ok': True}), events)
        self.assertEqual(os.readlink(running / 'Contents/Resources/current.txt'), 'version.txt')

    def test_a_folder_that_cannot_be_written_says_so_with_the_manual_download(self):
        running = make_app(self.apps, '1.0.6', self.program)
        self.publish('1.0.7')
        self.apps.chmod(0o555)
        self.addCleanup(self.apps.chmod, 0o755)
        code, events, _ = self.run_update(running)
        self.assertEqual((code, events[-1]['event']), (1, 'error'))
        self.assertIn('cannot be written', events[-1]['message'])
        self.assertEqual(events[-1]['manual'], 'https://github.com/o/r/releases/tag/launcher-v1.0.7')
        self.assertEqual([e['event'] for e in events], ['error'])    # nothing was downloaded

    def test_no_release_list_no_update(self):
        running = make_app(self.apps, '1.0.6', self.program)
        (self.site / 'releases.json').write_text(json.dumps({'message': 'API rate limit exceeded'}))
        code, events, _ = self.run_update(running)
        self.assertEqual((code, events[-1]['event'], events[-1]['manual']), (1, 'error', None))


class Replacement(Served):
    """The script alone: what it does when the swap cannot be made."""

    def script(self, pid, new, app):
        base = self.support / 'launcher-update'
        base.mkdir(parents=True, exist_ok=True)
        with mock.patch.dict(os.environ, {'MNG_LAUNCHER_UPDATE_NO_OPEN': '1'}):
            lu.start_replacement(base, pid, new, app)
        result = base / 'result.json'
        for _ in range(200):
            if result.exists() and result.read_text().strip():
                return json.loads(result.read_text())
            time.sleep(0.05)
        self.fail('the replacement wrote no outcome: ' + (base / 'replace.log').read_text())

    def quit_launcher(self):
        p = subprocess.Popen(['/bin/sleep', '0.2'])
        threading.Thread(target=p.wait, daemon=True).start()
        return p.pid

    def test_it_waits_for_the_launcher_to_quit(self):
        running = make_app(self.apps, '1.0.6', self.program)
        new = make_app(self.made, '1.0.7', self.program)
        launcher = subprocess.Popen(['/bin/sleep', '1.5'])
        started = time.time()
        threading.Thread(target=launcher.wait, daemon=True).start()
        self.assertEqual(self.script(launcher.pid, new, running), {'ok': True})
        self.assertGreater(time.time() - started, 1.3)
        self.assertEqual(version_of(running), '1.0.7')

    def test_nothing_is_left_beside_the_app_when_the_support_folder_is_on_another_disk(self):
        # the new app is then unpacked into a hidden folder beside the app (work_folder); 1.0.7 left that folder there,
        # empty, after every update (seen in /Applications with the support folder on an external disk)
        running = make_app(self.apps, '1.0.6', self.program)
        base = self.support / 'launcher-update'
        base.mkdir(parents=True)
        real = os.stat

        def other_disk(path, *a, **kw):
            st = real(path, *a, **kw)
            return os.stat_result((*st[:2], st.st_dev + 1, *st[3:])) if Path(path) == self.apps else st

        with mock.patch.object(lu.os, 'stat', other_disk):
            self.assertEqual(lu.work_folder(self.support, running), (base, self.apps / '.MNG Launcher update'))
        with mock.patch.object(lu.os, 'stat', real):
            self.assertEqual(lu.work_folder(self.support, running), (base, base))
        stage = self.apps / '.MNG Launcher update' / 'new'
        new = make_app(stage, '1.0.7', self.program)
        self.assertEqual(self.script(self.quit_launcher(), new, running), {'ok': True})
        self.assertEqual(version_of(running), '1.0.7')
        self.assertEqual([p.name for p in self.apps.iterdir()], [APP])
        self.assertTrue((base / 'replace.sh').is_file())             # the support folder is left as it is

    def test_a_missing_new_app_leaves_the_old_one(self):
        running = make_app(self.apps, '1.0.6', self.program)
        self.assertEqual(self.script(self.quit_launcher(), self.made / 'nothing.app', running), {'ok': False, 'why': 'missing'})
        self.assertEqual(version_of(running), '1.0.6')

    def test_the_old_app_comes_back_when_the_new_one_cannot_be_put_in_place(self):
        running = make_app(self.apps, '1.0.6', self.program)
        new = make_app(self.made, '1.0.7', self.program)
        self.made.chmod(0o555)                                       # the new app cannot leave its folder
        self.addCleanup(self.made.chmod, 0o755)
        self.assertEqual(self.script(self.quit_launcher(), new, running), {'ok': False, 'why': 'place'})
        self.assertEqual(version_of(running), '1.0.6')
        self.assertEqual([p.name for p in self.apps.iterdir()], [APP])


class Rules(unittest.TestCase):
    def release(self, version, host='https://github.com', **changes):
        name = f'MNG-Launcher-{version}-macOS-arm64.zip'
        asset = {'name': name, 'size': 1000, 'browser_download_url': f'{host}/o/r/releases/download/launcher-v{version}/{name}'}
        asset.update(changes)
        return {'tag_name': f'launcher-v{version}', 'draft': False, 'html_url': f'https://github.com/o/r/releases/tag/launcher-v{version}',
                'assets': [{'name': 'notes.txt', 'size': 3, 'browser_download_url': 'https://github.com/o/r/notes.txt'}, asset]}

    def test_the_highest_release_with_the_apps_zip_on_github(self):
        listing = [self.release('1.0.9'), self.release('1.0.10'), {'tag_name': 'source-20261118-abc', 'assets': []},
                   dict(self.release('1.1.0'), draft=True), self.release('1.0.11', host='https://example.com'),
                   self.release('1.0.12', size=0), self.release('1.0.13', size=lu.MAX_ZIP + 1), self.release('1.0.14', name='other.zip'),
                   self.release('1.0.15', host='http://github.com'), self.release('1.0.16', host='https://github.com.example.com')]
        found = lu.newest(listing)
        self.assertEqual((found['version'], found['size']), ('1.0.10', 1000))
        self.assertEqual(found['url'], 'https://github.com/o/r/releases/download/launcher-v1.0.10/MNG-Launcher-1.0.10-macOS-arm64.zip')
        self.assertEqual(found['manual'], 'https://github.com/o/r/releases/tag/launcher-v1.0.10')
        self.assertIsNone(lu.newest([self.release('1.0.9', host='http://127.0.0.1:8000')]))          # this Mac's server:
        self.assertEqual(lu.newest([self.release('1.0.9', host='http://127.0.0.1:8000')], local=True)['version'], '1.0.9')   # tests only
        self.assertIsNone(lu.newest({'message': 'API rate limit exceeded'}))

    def test_a_developer_id_launcher_requires_its_team_and_a_notarized_app(self):
        own = {'identifier': BUNDLE_ID, 'team': 'CSL9L993H3', 'requirement': f'identifier "{BUNDLE_ID}"'}
        self.assertEqual(lu.required_of_update(own),
                         f'(identifier "{BUNDLE_ID}") and anchor apple generic and certificate leaf[subject.OU] = "CSL9L993H3"')
        self.assertEqual(lu.required_of_update(dict(own, team='')), f'identifier "{BUNDLE_ID}"')
        for bad in (dict(own, requirement=''), dict(own, identifier=''), dict(own, team='x" or anchor apple')):
            with self.assertRaises(lu.UpdateError):
                lu.required_of_update(bad)

        with tempfile.TemporaryDirectory() as d:
            app = Path(d) / APP
            (app / 'Contents').mkdir(parents=True)
            with open(app / 'Contents/Info.plist', 'wb') as f:
                plistlib.dump({'CFBundleIdentifier': BUNDLE_ID, 'CFBundleShortVersionString': '1.0.7'}, f)
            calls = []

            def tool(answers):
                def run(cmd, **kw):
                    calls.append(cmd)
                    code, out = answers[Path(cmd[0]).name]
                    return subprocess.CompletedProcess(cmd, code, out, '')
                return run

            accepted = {'codesign': (0, ''), 'spctl': (0, f'{app}: accepted\nsource=Notarized Developer ID\n')}
            with mock.patch.object(lu.subprocess, 'run', tool(accepted)):
                lu.check_app(app, own, '1.0.7', (1, 0, 6))
            self.assertEqual([Path(c[0]).name for c in calls], ['codesign', 'codesign', 'spctl'])
            self.assertIn('-R=' + lu.required_of_update(own), calls[1])
            for answer in ((0, f'{app}: accepted\nsource=Developer ID\n'), (3, f'{app}: rejected\n')):   # signed, not notarized
                with mock.patch.object(lu.subprocess, 'run', tool(dict(accepted, spctl=answer))), self.assertRaises(lu.UpdateError):
                    lu.check_app(app, own, '1.0.7', (1, 0, 6))
            calls.clear()
            with mock.patch.object(lu.subprocess, 'run', tool({'codesign': (0, '')})):               # a build from a checkout
                lu.check_app(app, dict(own, team=''), '1.0.7', None)
            self.assertEqual([Path(c[0]).name for c in calls], ['codesign', 'codesign'])

    def test_how_a_launcher_is_signed_is_read_from_codesign(self):
        release = ('Executable=/Applications/MNG Launcher.app/Contents/MacOS/MNGBuilder\nIdentifier=org.macnativegames.launcher\n'
                   'Authority=Developer ID Application: SOMEONE (CSL9L993H3)\nTeamIdentifier=CSL9L993H3\n'
                   'designated => identifier "org.macnativegames.launcher" and anchor apple generic and certificate '
                   '1[field.1.2.840.113635.100.6.2.6] /* exists */ and certificate leaf[field.1.2.840.113635.100.6.1.13] /* exists */ '
                   'and certificate leaf[subject.OU] = CSL9L993H3\n')
        local = 'Identifier=org.macnativegames.launcher\nSignature=adhoc\nTeamIdentifier=not set\n# designated => identifier "org.macnativegames.launcher"\n'
        for text, team in ((release, 'CSL9L993H3'), (local, '')):
            with mock.patch.object(lu.subprocess, 'run', lambda cmd, **kw: subprocess.CompletedProcess(cmd, 0, '', text)):
                own = lu.signing('/x.app')
            self.assertEqual((own['identifier'], own['team']), (BUNDLE_ID, team))
            self.assertTrue(own['requirement'].startswith('identifier "org.macnativegames.launcher"'))
            self.assertNotIn('/*', own['requirement'])

    def test_a_translocated_copy_is_not_replaced(self):
        path = '/private/var/folders/ab/T/AppTranslocation/1F2E/d/MNG Launcher.app'
        self.assertIn('temporary copy', lu.not_replaceable(path))


class Window(unittest.TestCase):
    """The button: at the home page's top right, only with something to update to (tests excepted), off during a build."""

    def setUp(self):
        self.update = (SWIFT / 'LauncherUpdate.swift').read_text()
        self.home = (SWIFT / 'HomePage.swift').read_text()

    def test_the_button_is_the_last_thing_in_the_home_pages_top_row(self):
        row = self.home[self.home.index('Text("MNG Launcher").font(.largeTitle'):self.home.index('LauncherUpdateFailure()')]
        self.assertRegex(row, r'Spacer\(\)\s*(//[^\n]*\n\s*)*Button \{ model\.page = "terminal" \}[^\n]*\n\s*(//[^\n]*\n\s*)*LauncherUpdateButton\(\)\s*\}')

    def test_it_shows_only_with_a_newer_release_or_in_a_test(self):
        offer = self.update[self.update.index('var launcherUpdateOffer: String?'):self.update.index('var launcherUpdateBlocked')]
        self.assertIn('if let newer = newerLauncher { return newer.version }', offer)
        self.assertIn('if LauncherUpdate.test != nil { return "" }', offer)
        self.assertIn('if case .failed = launcherUpdate { return "" }', offer)       # to be pressed again
        self.assertIn('return nil', offer.split('if case .failed')[1])                # nothing else offers one
        self.assertIn('if let version = model.launcherUpdateOffer {', self.update)   # the view draws nothing without one
        self.assertIn('ProcessInfo.processInfo.environment["MNG_LAUNCHER_UPDATE_TEST"]', self.update)
        model = (SWIFT / 'Model.swift').read_text()                                   # newerLauncher: releases only, higher
        self.assertIn('guard Paths.isRelease,', model)
        self.assertIn('version.compare(own, options: .numeric) == .orderedDescending', model)

    def test_a_build_keeps_the_launcher_where_it_is(self):
        self.assertIn('var launcherUpdateBlocked: Bool { jobRunning }', self.update)
        self.assertIn('guard !launcherUpdateRunning, !launcherUpdateBlocked else { return }', self.update)
        self.assertIn('.disabled(model.launcherUpdateBlocked)', self.update)

    def test_the_window_runs_the_apps_own_script_and_quits_when_told(self):
        self.assertIn('Bundle.main.resourceURL?.appendingPathComponent("launcher_update.py")', self.update)
        self.assertIn('"--pid", String(ProcessInfo.processInfo.processIdentifier)', self.update)
        self.assertIn('"--app", Bundle.main.bundleURL.path', self.update)
        install = self.update[self.update.index('case "install":'):self.update.index('case "error":')]
        self.assertIn('NSApp.terminate(nil)', install)
        self.assertEqual(self.update.count('NSApp.terminate'), 1)                    # nowhere else: not on an error
        # the tests' switches change what is offered and which list is read, never a check
        self.assertIn('if let feed = LauncherUpdate.testFeed { arguments += ["--feed", feed] }', self.update)
        self.assertIn('if LauncherUpdate.test != nil { arguments.append("--same") }', self.update)
        # every event the script prints has a case
        for event in ('found', 'download', 'verify', 'install', 'error'):
            self.assertIn(f"emit('{event}'", SCRIPT.read_text())
            self.assertIn(f'case "{event}":', self.update)

    def test_a_release_carries_the_script_in_the_signed_app(self):
        make = (ROOT / 'tools/builder_app/make_app.py').read_text()
        self.assertIn("for name in ('bundled_job.py', 'source_updates.py', 'launcher_update.py'):", make)
        # committed with the Launcher it goes into; not asked of a source release, which may be of a commit before it
        required = make[make.index('required = {'):make.index('missing = required - names')]
        self.assertIn("if commit == 'HEAD':", required)
        self.assertIn("required.add('tools/builder_app/launcher_update.py')", required.split("if commit == 'HEAD':")[1])
        self.assertNotIn('launcher_update.py', required.split("if commit == 'HEAD':")[0])
        make_app = importlib.import_module('tools.builder_app.make_app')
        with tempfile.TemporaryDirectory() as d:
            git = lambda *a: subprocess.run(['git', '-C', d, *a], check=True, capture_output=True)
            git('init', '-q')
            for name in ('build_app.py', 'tools/builder_jobs/__main__.py', 'tools/builder_app/bundled_job.py',
                         'tools/builder_app/source_updates.py', 'tools/builder_app/source_signing.pub',
                         'games/totk/third_party/d2r_gfx/gfx/nvn/totk_seed_image.h',
                         'games/oot/ios/tools/seed_image.py', 'games/totk/ios/tools/seed_image.py'):
                (Path(d) / name).parent.mkdir(parents=True, exist_ok=True)
                (Path(d) / name).write_text('# fixture\n')
            git('add', '.')
            git('-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', '-c', 'commit.gpgsign=false', 'commit', '-qm', 'before the updater')
            before = subprocess.check_output(['git', '-C', d, 'rev-parse', 'HEAD'], text=True).strip()
            make_app.check_release_source(d, before)                   # a source release of that commit
            with self.assertRaisesRegex(ValueError, 'launcher_update.py'):
                make_app.check_release_source(d)                       # a Launcher from it: the updater is missing


@unittest.skipUnless(os.environ.get('MNG_TEST_LAUNCHER_RELEASES'), 'MNG_TEST_LAUNCHER_RELEASES names no signed releases')
class RealReleases(Served):
    """Two signed, notarized releases of this Mac's release folder (<version>/release/MNG Launcher.app and its ZIP):
    the older one updates to the newer one, and refuses an ad-hoc app that says the newer version."""

    def setUp(self):
        super().setUp()
        root = Path(os.environ['MNG_TEST_LAUNCHER_RELEASES'])
        found = sorted((lu.su.version_tuple(p.name), p) for p in root.iterdir()
                       if lu.version_or_none(p.name) and (p / 'release' / APP).is_dir()
                       and (p / 'release' / f'MNG-Launcher-{p.name}-macOS-arm64.zip').is_file())
        if len(found) < 2:
            self.skipTest(f'{root} holds fewer than two releases with their ZIP')
        (_, self.old), (_, self.new) = found[-2], found[-1]

    def serve(self, version, archive):
        name = f'MNG-Launcher-{version}-macOS-arm64.zip'
        os.link(archive, self.site / name) if os.stat(archive).st_dev == os.stat(self.site).st_dev else shutil.copy2(archive, self.site / name)
        (self.site / 'releases.json').write_text(json.dumps([{
            'tag_name': f'launcher-v{version}', 'draft': False, 'html_url': f'https://github.com/o/r/releases/tag/launcher-v{version}',
            'assets': [{'name': name, 'size': (self.site / name).stat().st_size, 'browser_download_url': f'{self.base}/{name}'}]}]))

    def test_a_release_updates_to_the_next_and_refuses_an_unsigned_one(self):
        running = self.apps / APP
        subprocess.run(['/usr/bin/ditto', str(self.old / 'release' / APP), str(running)], check=True)
        self.assertTrue(lu.signing(running)['team'])
        # an app that says the newer version but is not this team's
        fake = make_app(self.made, self.new.name, self.program)
        self.publish(self.new.name, app=fake)
        code, events, _ = self.run_update(running)
        self.assertEqual((code, events[-1]['event']), (1, 'error'), events)
        self.assertIn('not signed as this Launcher is', events[-1]['message'])
        self.assertEqual(version_of(running), self.old.name)
        # the real one
        (self.site / f'MNG-Launcher-{self.new.name}-macOS-arm64.zip').unlink()
        self.serve(self.new.name, self.new / 'release' / f'MNG-Launcher-{self.new.name}-macOS-arm64.zip')
        code, events, outcome = self.run_update(running)
        self.assertEqual((code, outcome), (0, {'ok': True}), events)
        self.assertEqual(version_of(running), self.new.name)
        self.assertEqual(subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(running)]).returncode, 0)
        gate = subprocess.run(['/usr/sbin/spctl', '--assess', '--type', 'execute', '--verbose=2', str(running)], capture_output=True, text=True)
        self.assertIn('Notarized Developer ID', gate.stdout + gate.stderr)


if __name__ == '__main__':
    unittest.main()
