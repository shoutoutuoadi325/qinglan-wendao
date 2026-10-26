#!/usr/bin/env python3
"""Tool-dispatch contracts only: fake engine tools do not prove UE builds."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
STUB = '''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
with open(os.environ['STUB_LOG'], 'a') as f: f.write(json.dumps(sys.argv[1:]) + '\\n')
for output in json.loads(os.environ.get('STUB_OUTPUTS', '[]')):
    p = Path(output); p.parent.mkdir(parents=True, exist_ok=True); p.touch()
if os.environ.get('STUB_FAIL_MESH') and any('QinglanMeshImport' in arg for arg in sys.argv):
    print('QinglanMeshImport finished ok=1')
    raise SystemExit(7)
raise SystemExit(int(os.environ.get('STUB_RC', '0')))
'''


class Wrappers(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="qinglan contract's ")
        self.base = Path(self.tmp.name)
        self.engine = self.base / "Unreal engine's"
        self.project = self.base / "Game project's"
        self.project.mkdir()
        (self.project / 'QinglanAscension.uproject').touch()
        self.log = self.base / 'calls.jsonl'
        self.env = dict(os.environ, QINGLAN_ENGINE_ROOT=str(self.engine),
                        QINGLAN_PROJECT_DIR=str(self.project), STUB_LOG=str(self.log))
        self.env.pop('QINGLAN_PLATFORM', None)
        self.env.pop('QINGLAN_CONFIG', None)
        self.env.pop('QINGLAN_TARGET', None)
        self.env.pop('QINGLAN_ARCHIVE_DIR', None)

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, path, content=''):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def tool(self, path):
        self.file(path, STUB).chmod(0o755)

    def run_wrapper(self, name, **env):
        return subprocess.run(['bash', str(ROOT / 'tools/build' / name)], env=dict(self.env, **env),
                              text=True, capture_output=True)

    def calls(self):
        return [json.loads(x) for x in self.log.read_text().splitlines()]

    def host_editor(self, host):
        if host == 'Linux':
            self.tool(self.engine / 'Engine/Build/BatchFiles/Linux/Build.sh')
            self.file(self.engine / 'Engine/Binaries/Linux/UnrealEditor')
            self.file(self.project / 'Binaries/Linux/libUnrealEditor-QinglanAscension.so')
        elif host == 'Mac':
            self.tool(self.engine / 'Engine/Build/BatchFiles/Mac/Build.sh')
            self.file(self.engine / 'Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor')
            self.file(self.project / 'Binaries/Mac/UnrealEditor-QinglanAscension.dylib')

    def test_linux_and_mac_build_dispatch_and_literal_paths(self):
        for host in ('Linux', 'Mac'):
            with self.subTest(host=host):
                self.host_editor(host)
                r = self.run_wrapper('build-game.sh', QINGLAN_HOST_PLATFORM=host)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(self.calls()[-1], ['QinglanAscensionEditor', host, 'Development',
                    '-project=' + str(self.project / 'QinglanAscension.uproject'), '-waitmutex'])

    def test_cross_target_game_uses_host_build_tool(self):
        self.host_editor('Linux')
        self.file(self.project / 'Binaries/Win64/QinglanAscension-Win64-Shipping.exe')
        r = self.run_wrapper('build-game.sh', QINGLAN_HOST_PLATFORM='Linux', QINGLAN_PLATFORM='Win64',
                             QINGLAN_TARGET='QinglanAscension', QINGLAN_CONFIG='Shipping')
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_wrapper('build-game.sh', QINGLAN_HOST_PLATFORM='Linux', QINGLAN_PLATFORM='Mac')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('editor target must match', r.stderr)

    def test_packaging_layouts_and_artifacts(self):
        self.tool(self.engine / 'Engine/Build/BatchFiles/RunUAT.sh')
        for platform, client in (
                ('Linux', 'QinglanAscension/Binaries/Linux/QinglanAscension-Linux-Shipping'),
                ('Win64', 'Windows/QinglanAscension/Binaries/Win64/QinglanAscension-Win64-Shipping.exe')):
            with self.subTest(platform=platform):
                archive = self.base / (platform + " archive's")
                binary = self.file(archive / client)
                pak = self.file(archive / 'Content/Paks/QinglanAscension.pak')
                r = self.run_wrapper('package-game.sh', QINGLAN_HOST_PLATFORM='Mac',
                                     QINGLAN_PLATFORM=platform, QINGLAN_ARCHIVE_DIR=str(archive),
                                     QINGLAN_DDC='(ProjectPak,EnginePak,Local)')
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn('-archivedirectory=' + str(archive), self.calls()[-1])
                self.assertIn('-ddc=(ProjectPak,EnginePak,Local)', self.calls()[-1])
                binary.unlink()
                r = self.run_wrapper('package-game.sh', QINGLAN_HOST_PLATFORM='Mac',
                                     QINGLAN_PLATFORM=platform, QINGLAN_ARCHIVE_DIR=str(archive))
                self.assertNotEqual(r.returncode, 0)
                self.assertIn('no ' + platform + ' client', r.stderr)
                self.file(binary)
                pak.unlink()
                r = self.run_wrapper('package-game.sh', QINGLAN_HOST_PLATFORM='Mac',
                                     QINGLAN_PLATFORM=platform, QINGLAN_ARCHIVE_DIR=str(archive))
                self.assertNotEqual(r.returncode, 0)
                self.assertIn('no pak', r.stderr)

    def test_mac_packaging_archives_the_complete_staged_bundle(self):
        # UAT's -archive copies only the pre-stage app from Binaries/Mac. The
        # wrapper must skip that archive and copy the staged bundle (with the
        # cooked payload) instead, otherwise the delivered app cannot boot.
        self.tool(self.engine / 'Engine/Build/BatchFiles/RunUAT.sh')
        archive = self.base / "Mac archive's"
        staged = self.project / 'Saved/StagedBuilds/Mac/QinglanAscension.app'  # Development default: no config suffix
        client = 'Contents/MacOS/QinglanAscension-Mac-Shipping'
        pak = 'Contents/UE/QinglanAscension/Content/Paks/QinglanAscension-Mac.pak'
        self.file(staged / client)
        self.file(staged / pak)
        # Pre-existing shell from the Xcode build step must be replaced.
        self.file(archive / 'Mac' / Path(staged).name / client)

        r = self.run_wrapper('package-game.sh', QINGLAN_HOST_PLATFORM='Mac',
                             QINGLAN_PLATFORM='Mac', QINGLAN_ARCHIVE_DIR=str(archive),
                             QINGLAN_DDC='(ProjectPak,EnginePak,Local)')
        self.assertEqual(r.returncode, 0, r.stderr)
        call = self.calls()[-1]
        self.assertNotIn('-archive', call)
        self.assertFalse(any(str(arg).startswith('-archivedirectory=') for arg in call))
        self.assertIn('-ddc=(ProjectPak,EnginePak,Local)', call)
        archived = archive / 'Mac' / Path(staged).name
        self.assertTrue((archived / client).exists())
        self.assertTrue((archived / pak).exists())

        (staged / pak).unlink()
        r = self.run_wrapper('package-game.sh', QINGLAN_HOST_PLATFORM='Mac',
                             QINGLAN_PLATFORM='Mac', QINGLAN_ARCHIVE_DIR=str(archive))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('no pak', r.stderr)
        self.file(staged / pak)
        shutil.rmtree(staged)
        r = self.run_wrapper('package-game.sh', QINGLAN_HOST_PLATFORM='Mac',
                             QINGLAN_PLATFORM='Mac', QINGLAN_ARCHIVE_DIR=str(archive))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('no Mac app', r.stderr)

    def test_nonzero_tool_and_missing_outputs_fail(self):
        self.host_editor('Mac')
        r = self.run_wrapper('build-game.sh', QINGLAN_HOST_PLATFORM='Mac', STUB_RC='37')
        self.assertEqual(r.returncode, 37)
        shutil.rmtree(self.project / 'Binaries')
        r = self.run_wrapper('build-game.sh', QINGLAN_HOST_PLATFORM='Mac')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('no project modules', r.stderr)

    def test_missing_engine_fails_before_tool_execution(self):
        r = self.run_wrapper('build-game.sh', QINGLAN_HOST_PLATFORM='Mac')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('engine build script missing', r.stderr)
        self.assertFalse(self.log.exists())

    def raw_sources(self):
        raw = self.base / "Raw assets' source"
        self.file(raw / 'quaternius/hero.fbx')
        for audio in ('music/music_boss_epic_boss_battle_loop', 'music/music_combat_alt_prepare_your_swords',
                      'music/music_combat_fast_fight_loop', 'music/music_hub_calm_orient_peace_valley',
                      'sfx/sfx_sword_swing', 'sfx/sfx_hit_impact', 'sfx/sfx_technique_release', 'sfx/sfx_telegraph_warn'):
            self.file(raw / ('audio/original/' + audio + '.wav'))
        return raw

    def test_content_argument_boundaries_and_assetsraw(self):
        raw = self.raw_sources()
        editor = self.engine / 'Engine/Binaries/Mac/UnrealEditor-Cmd.app/Contents/MacOS/UnrealEditor-Cmd'
        self.tool(editor)
        # Deliberately no outputs: command execution can succeed, acceptance must fail.
        r = self.run_wrapper('regenerate-content.sh', QINGLAN_HOST_PLATFORM='Mac',
                             QINGLAN_ASSETS_RAW=str(raw), QINGLAN_LOW_QUALITY='1',
                             QINGLAN_DDC='(ProjectPak,EnginePak,Local)')
        self.assertNotEqual(r.returncode, 0)
        calls = self.calls()
        self.assertEqual(len(calls), 6)
        self.assertTrue(all(x[0] == str(self.project / 'QinglanAscension.uproject') for x in calls))
        self.assertTrue(all('-DDC=(ProjectPak,EnginePak,Local)' in x for x in calls))
        for name in ('QinglanMeshImport', 'QinglanContentBootstrap', 'QinglanAudioBootstrap'):
            call = next(x for x in calls if any(name in arg for arg in x))
            self.assertIn('-assetsraw=' + str(raw), call)
        self.assertIn('-lowquality', next(x for x in calls if any('ContentBootstrap' in arg for arg in x)))

    def test_mesh_completion_markers_cannot_mask_nonzero_exit(self):
        raw = self.raw_sources()
        self.tool(self.engine / 'Engine/Binaries/Mac/UnrealEditor-Cmd')
        self.file(self.project / 'Saved/Logs/QinglanAscension.log', 'QinglanMeshImport finished ok=1\n')
        outputs = ['Data/Techniques/DA_Tech_stub.uasset', 'Characters/SK_Stub.uasset',
                   'Input/IMC_Default.uasset', 'Data/ST_Game.uasset', 'Data/DT_Localization.uasset',
                   'Materials/M_QinglanSolid.uasset']
        outputs += ['Maps/' + x + '.umap' for x in ('L_Hub', 'L_BambooHollow', 'L_AshenVerge', 'L_TribulationArena')]
        outputs += ['VFX/' + x + '.uasset' for x in ('NS_Impact', 'NS_Telegraph', 'NS_Cast')]
        outputs += ['Characters/' + x + '.uasset' for x in ('Human/ABP_Human', 'Knight/ABP_Knight',
                    'Monsters/ABP_Slime', 'Monsters/ABP_Skeleton', 'Monsters/ABP_Bat', 'Monsters/ABP_Dragon')]
        outputs += ['Audio/A' + str(i) + '.uasset' for i in range(8)]
        outputs += ['Textures/T' + str(i) + '.uasset' for i in range(8)]
        outputs += ['Materials/' + x + suffix + '.uasset' for x in ('M_BambooWall', 'M_Cobblestone',
                    'M_CobblestoneAlt', 'M_StonePath', 'M_GrassRock', 'M_Cliff', 'M_DirtFloor',
                    'M_VolcanicRock', 'M_RoofTiles') for suffix in ('', '_Solid')]
        env = dict(QINGLAN_HOST_PLATFORM='Mac', QINGLAN_ASSETS_RAW=str(raw),
                   STUB_OUTPUTS=json.dumps([str(self.project / 'Content/Qinglan' / x) for x in outputs]))
        success = self.run_wrapper('regenerate-content.sh', **env)
        self.assertEqual(success.returncode, 0, success.stderr)
        failed = self.run_wrapper('regenerate-content.sh', STUB_FAIL_MESH='1', **env)
        self.assertNotEqual(failed.returncode, 0)
        self.assertNotIn('continuing', failed.stdout)

    def test_windows_batch_dispatch_quotes_and_uses_native_paths(self):
        fake_bin = self.base / 'bin'
        self.tool(fake_bin / 'cmd.exe')
        cygpath = self.file(fake_bin / 'cygpath', '#!/usr/bin/env bash\nprintf "C:%s\\n" "$2"\n')
        cygpath.chmod(0o755)
        self.file(self.engine / 'Engine/Build/BatchFiles/Build.bat')
        self.file(self.engine / 'Engine/Binaries/Win64/UnrealEditor.exe')
        self.file(self.project / 'Binaries/Win64/UnrealEditor-QinglanAscension.dll')
        r = self.run_wrapper('build-game.sh', QINGLAN_HOST_PLATFORM='Win64', PATH=str(fake_bin) + os.pathsep + os.environ['PATH'])
        self.assertEqual(r.returncode, 0, r.stderr)
        args = self.calls()[-1]
        self.assertEqual(args[:4], ['/d', '/v:off', '/s', '/c'])
        self.assertIn('"-project=C:' + str(self.project / 'QinglanAscension.uproject') + '"', args[-1])
        self.assertTrue(args[-1].startswith('""C:'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
