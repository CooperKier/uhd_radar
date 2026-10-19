"""Exercise runner subprocess cleanup and result saving without radio hardware."""
import importlib
from pathlib import Path
import subprocess
import sys
from unittest.mock import Mock

import pytest
from ruamel.yaml import YAML


RUNNERS = [importlib.import_module(name) for name in ('run', 'runRx', 'runTx')]
ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize('module', RUNNERS, ids=lambda module: module.__name__)
@pytest.mark.parametrize('mode', ['rx', 'tx', 'tx_split'])
def test_run_without_optional_gps_tools(module, mode, tmp_path, monkeypatch):
    yaml = YAML()
    receive = mode == 'rx'
    config_name = 'Rx_B206mini.yaml' if receive else 'Tx_B206mini.yaml'
    config = yaml.load(ROOT / 'config' / config_name)
    config['FILES']['output_dir'] = str(tmp_path / 'data')
    if mode == 'tx_split':
        config['FILES']['max_chirps_per_file'] = 100
        config['RUN_MANAGER']['final_save_loc'] = str(tmp_path / 'merged.bin')
    config_path = tmp_path / 'config.yaml'
    with config_path.open('w') as stream:
        yaml.dump(config, stream)
    monkeypatch.chdir(tmp_path)
    (tmp_path / 'data').mkdir()
    # These stale logs must not be attached when GPS commands are unavailable.
    (tmp_path / 'gpspipe_stdout.log').write_text('old GPS log')
    (tmp_path / 'track.gpx').write_text('old GPX log')

    real_popen = subprocess.Popen
    def start_process(args, **kwargs):
        if args[0] in ('gpspipe', 'gpxlogger'):
            raise FileNotFoundError(args[0])
        assert args[0] == './radar'
        program = 'print("radar completed")\n'
        if receive:
            program += 'from pathlib import Path; Path("data/rx_samps.bin").write_bytes(b"samples")\n'
        kwargs['cwd'] = tmp_path
        return real_popen([sys.executable, '-c', program], **kwargs)

    monkeypatch.setattr(module.subprocess, 'Popen', start_process)
    monkeypatch.setattr(module.time, 'sleep', lambda seconds: None)
    runner = module.RadarProcessRunner(str(config_path), output_to_stdout=False)
    runner.config = config
    runner.setup_complete = True
    runner.run()
    runner.uhd_process.wait(timeout=10)
    prefix = runner.stop()

    assert Path(prefix + '_config.yaml').is_file()
    assert Path(prefix + '_uhd_stdout.log').read_text().endswith('\tradar completed\n')
    assert not Path(prefix + '_gpspipe_stdout.log').exists()
    assert not Path(prefix + '_track.gpx').exists()
    if receive:
        assert Path(prefix + '_rx_samps.bin').read_bytes() == b'samples'
        assert not (tmp_path / 'data/rx_samps.bin').exists()
    else:
        assert not Path(prefix + '_rx_samps.bin').exists()
        assert not (tmp_path / 'merged.bin').exists()
    assert runner.stop() == 0  # Repeated cleanup must be harmless.


@pytest.mark.parametrize('module', RUNNERS, ids=lambda module: module.__name__)
def test_failed_radar_retains_original_failure(module):
    runner = module.RadarProcessRunner('unused.yaml')
    runner.is_running = True
    runner.uhd_process = Mock(returncode=255)
    runner.uhd_process.poll.return_value = 255
    runner.uhd_output_reader_thread = Mock()
    with pytest.raises(RuntimeError, match='Radar exited with status 255'):
        runner.stop()
    runner.uhd_process.send_signal.assert_not_called()
    runner.uhd_output_reader_thread.join.assert_called_once()


@pytest.mark.parametrize('module', RUNNERS, ids=lambda module: module.__name__)
def test_optional_processes_are_reaped(module, monkeypatch):
    runner = module.RadarProcessRunner('unused.yaml')
    runner.is_running = True
    runner.config = {'RF0': {'receive': False}, 'RUN_MANAGER': {}, 'FILES': {}}
    runner.uhd_process = Mock(returncode=0)
    runner.uhd_process.poll.return_value = 0
    runner.uhd_output_reader_thread = Mock()
    runner.gpspipe_process = Mock()
    runner.gpspipe_process.poll.return_value = None
    runner.gpxlogger_process = Mock()
    runner.gpxlogger_process.poll.return_value = 0
    monkeypatch.setattr(module, 'save_data', Mock(return_value='saved'))
    assert runner.stop() == 'saved'
    runner.gpspipe_process.kill.assert_called_once()
    runner.gpspipe_process.wait.assert_called_once()
    runner.gpxlogger_process.kill.assert_not_called()
    runner.gpxlogger_process.wait.assert_called_once()
