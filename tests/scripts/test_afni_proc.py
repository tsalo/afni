from pathlib import Path
from .utils import tools

ft_dir = Path("AFNI_data6/FT_analysis/FT")
data_paths = {
    "anat": ft_dir / "FT_anat+orig.HEAD",
    "epi_1": ft_dir / "FT_epi_r1+orig.HEAD",
    "epi_2": ft_dir / "FT_epi_r2+orig.HEAD",
    "epi_3": ft_dir / "FT_epi_r3+orig.HEAD",
    "av1_vis": ft_dir / "AV1_vis.txt",
    "av2_aud": ft_dir / "AV2_aud.txt",
}


def test_handout_realcase2(data, run_cmd):
    """Test the command in the afni_proc.py handout (real case 2)."""
    subj = "FT"

    cmd = """
    afni_proc.py
        -subj_id {subj}
        -script proc.{subj}
        -scr_overwrite
        -blocks tshift align tlrc volreg blur mask scale regress
        -copy_anat {data.anat}
        -dsets {data.epi_1} {data.epi_2} {data.epi_3}
        -volreg_align_to MIN_OUTLIER
        -volreg_align_e2a
        -volreg_tlrc_warp
        -blur_size 4.0
        -tcat_remove_first_trs 2
        -regress_stim_times {data.av1_vis} {data.av2_aud}
        -regress_stim_labels vis aud
        -regress_basis 'BLOCK(20,1)'
        -regress_censor_motion 0.3
        -regress_opts_3dD
        -jobs 2
        -gltsym 'SYM: vis
        -aud'
        -glt_label 1 V
        -A
        -gltsym 'SYM: 0.5*vis +0.5*aud'
        -glt_label 2 mean.VA
        -regress_compute_fitts
        -regress_make_ideal_sum sum_ideal.1D
        -regress_est_blur_epits
        -regress_est_blur_errts
        -regress_run_clustsim yes
    """
    run_cmd(cmd, locals(), workdir=data.outdir)

    # test outputs if above commands ran
    tools.assert_all_files_equal(
        data,
        text_file_patterns=[".FT"],
        kwargs_text_files={"ignore_patterns": ["auto-gener"]},
    )


def test_handout_realcase3(data, run_cmd):
    subj = "FT"
    cmd = """
    afni_proc.py
        -subj_id {subj}
        -script proc.{subj}
        -scr_overwrite
        -blocks tshift align tlrc volreg blur mask scale regress
        -copy_anat {data.anat}
        -dsets {data.epi_1} {data.epi_2} {data.epi_3}
        -volreg_align_to MIN_OUTLIER
        -volreg_align_e2a
        -volreg_tlrc_warp
        -blur_size 4.0
    """
    run_cmd(cmd, locals(), workdir=data.outdir)

    # test outputs if above commands ran

    tools.assert_all_files_equal(
        data,
        text_file_patterns=[".FT"],
        kwargs_text_files={"ignore_patterns": ["auto-gener"]},
    )
