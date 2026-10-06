# Furioz420 core main reconciliation

The previous `Furioz420/wxl-core` `main` head was `79948a4` (July 2026). It
predated the organization `v1.1` extension architecture and kept module sources
under `scripts/`, along with old host and PR-patch snapshots. The eight module
repositories in this publication pass use separate extension DLLs instead.

This branch starts from current public WarcraftXL `v1.1` commit `60033ab`,
adds the compatible core SDK/build contracts in commits `644a44c` and
`48b2849`, then merges the old fork `main` as a second parent using the Git
`ours` strategy. That merge retains the old fork's complete history while the
checked-out files remain the tested `v1.1` core tree. It is a fast-forward path
for the owned fork's `main`, without a force push or copying the legacy
`scripts/` layout into the new core.

Review the large tree diff against old `main` as a repository-layout update,
not as a claim that every old experimental script was ported. The old source
remains available at `79948a4`; supported modules live in their respective
repositories. The new core tree includes no private Challenges or Garrison
module source.

Before promoting a separately built client package, pin its module workflows
to core commit `48b2849ed05d2c66e2ba2a6185e09fafd111da53`, check their
Win32 CI builds, and run the matching-client smoke route. See
`ExtensionModuleCompatibility.md` for build evidence and limits.
