import pytest


@pytest.mark.slow
def test_with_afni_system_check(data, run_cmd):
    cmd = "afni_system_check.py -check_all"
    run_cmd(cmd, locals())
