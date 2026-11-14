"""The standard-AppData migration preserves unknown overrides and private saves."""
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from build_windows_services import migrate_owned_local_save


def main():
    with tempfile.TemporaryDirectory() as folder:
        game = Path(folder)
        path = game / 'local_save.txt'
        assert migrate_owned_local_save(game) is None
        for unknown in [b'', b'custom', b'../profile/services\n']:
            path.write_bytes(unknown)
            try:
                migrate_owned_local_save(game)
            except ValueError:
                pass
            else:
                raise AssertionError('Unknown override accepted')
            assert path.read_bytes() == unknown and len(list(game.iterdir())) == 1
        path.write_bytes(b'../profile/services')
        result = migrate_owned_local_save(game)
        assert not path.exists() and Path(result['backup']).read_bytes() == b'../profile/services'
        assert migrate_owned_local_save(game) is None
    print('Windows services AppData migration: missing, exact-owned, unknown overrides passed')


if __name__ == '__main__':
    main()
