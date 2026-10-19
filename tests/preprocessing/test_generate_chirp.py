from ruamel.yaml import YAML
import sys
sys.path.append("preprocessing")
from generate_chirp import generate_chirp
from pathlib import Path
import pytest
import numpy as np
from generate_chirp import generate_from_yaml_filename

'''
All unit test functions must start with test_ and be in a file named test_*.py within the tests folder!
'''


def test_chirp_length():
    """ Tests the length of the chirp without zero-padding in default.yaml (20e-6 seconds) """
    # Load config file
    yaml_filename = Path(__file__).resolve().parent.parent.parent/"config"/"default.yaml"
    yaml = YAML(typ='safe')
    stream = open(yaml_filename)
    config = yaml.load(stream)

    # Create the chirp
    time_samples, _ = generate_chirp(config)

    # Test if the output time matches the config
    config_chirp_length = config['GENERATE']['chirp_length'] # [s]
    sample_rate = config['GENERATE']['sample_rate'] # [Hz]
    actual_chirp_length = len(time_samples)/sample_rate  # [s]

    assert config_chirp_length == actual_chirp_length

if __name__ == "__main__":
    # Manually test functions here
    test_chirp_length()
    print("All tests passed!")


@pytest.mark.parametrize('filename', ['Rx_B206mini.yaml', 'Tx_B206mini.yaml'])
def test_b206_chirp_output_path(filename, tmp_path):
    yaml = YAML(typ='safe')
    config = yaml.load(Path(__file__).resolve().parents[2] / 'config' / filename)
    assert config['GENERATE']['show_plot'] is False
    config['FILES']['output_dir'] = str(tmp_path / 'output')
    local_config = tmp_path / filename
    with local_config.open('w') as stream:
        yaml.dump(config, stream)
    samples = generate_from_yaml_filename(str(local_config))
    output = tmp_path / 'output' / config['FILES']['chirp_loc']
    recovered = np.fromfile(output, dtype=np.float32)
    assert recovered.size == 2240  # 56 MS/s * 20 us * I/Q components
    assert np.array_equal(recovered, samples)
    assert np.isfinite(recovered).all()
    assert np.max(np.abs(recovered)) > 0
