# Upstream alignment and owned branch policy

Checked on 2026-10-06: WarcraftXL/wxl-core's `v1.2.296` tag, its
`v1.2.291` tag, its `v1.1.260` tag, and the `v1.1` branch all point to
`60033ab125e3b23ef5e73f0767c44d6ea053148e`. The owned
Furioz420/wxl-core `main` descends from that exact commit and adds the
extension SDK and core bindings documented in
[`ExtensionModuleCompatibility.md`](ExtensionModuleCompatibility.md).
There is no newer core source to port from `v1.2.296`; its higher version
number does not represent a newer commit than the baseline already used here.

The owned `main` is an extended build, not the unmodified upstream release.
Do not tag it `v1.2.296`: doing so would conflate the upstream release with
the owned SDK additions. The module CI workflows currently pin the compatible
core commit `48b2849ed05d2c66e2ba2a6185e09fafd111da53`, which remains
reachable from owned `main`.

Owned repository branches were consolidated to `main` after their previous
tips were saved in verified local Git bundles. The old experimental branches
are archived work, not automatically accepted runtime features. Three
superseded upstream PRs were closed with explanations: core #20, core #30,
and modern-M2 #4. Git tags were not deleted.

When WarcraftXL publishes a tag on a *new* commit, compare its commit and
tree against this baseline, port only compatible upstream changes into owned
`main`, rebuild the core and affected modules together, update module CI pins,
then smoke-test the resulting client binaries before a release.
