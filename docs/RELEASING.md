# Releasing ReXGlue SDK

## Branches

`main` always sits at the most recent stable tag. `development` is the working canary. All feature work merges here first. `release/X.Y.Z` is short-lived: branched from dev tip when a release is cut, merged into both dev and main when the release is published, then deleted.

## Versioning

The build version is computed automatically. Do not edit `CMakeLists.txt` to bump the version on a routine release.

`CMakeLists.txt` carries only the API floor as `PROJECT VERSION MAJOR.MINOR.0`. The patch field there is a placeholder CMake requires. Only major and minor mean anything. Edit the floor only when you are introducing a public API change.

Version format follows [CMake's `CMAKE_VERSION`](https://cmake.org/cmake/help/latest/variable/CMAKE_VERSION.html) convention: `MAJOR.MINOR[.PATCH[.TWEAK]][-id]`.

The full version is derived at configure time:

- On a tagged commit the version is the tag verbatim (minus the leading `v`). Tags may have two, three or four components (`v0.7`, `v0.7.5`, `v0.7.5.1`). Whatever you tag is the released version.
- On a `release/*` branch the version is `MAJOR.<floor-minor>.<tag-patch>.<commit-count>-rc.g<sha>`.
- On `development` or anywhere else the version is `MAJOR.<floor-minor>.<tag-patch>.<commit-count>-dev.g<sha>`.

When the floor major.minor is ahead of the most recent tag (a floor bump not yet released), the version instead holds at `MAJOR.<floor-minor>.0-<id>` with no commit-count tweak, and stays there until a release on that floor is published and `main` catches up.

`<tag-patch>` is the patch of the most recent tag. It is not pre-incremented, so the build tracks the released base plus commits and the next release tag is the next number. `<commit-count>` is the number of commits since that tag (the TWEAK slot). The `-id` trailer identifies the build channel (`dev` or `rc`) and the short commit SHA.

Examples:

| Scenario | Version |
| --- | --- |
| `git tag v0.7.5` pushed | `0.7.5` |
| `git tag v0.7.5.1` pushed (hotfix tweak) | `0.7.5.1` |
| `development`, 38 commits past `v0.7.4` | `0.7.4.38-dev.gb27f3f74` |
| `release/0.7.5`, 15 commits past `v0.7.4` | `0.7.4.15-rc.gdeadbee` |
| Floor bumped to `0.8`, 3 commits past `v0.7.5` | `0.8.0-dev.gdef5678` |

A version with `-dev` or `-rc` is unstable. Only a version without an `-id` trailer is covered by the public API.

## Release flow

### Cutting the release

Figure out the next release version. On a normal release it is `MAJOR.<floor-minor>.<last-tag-patch + 1>`. If the release introduces a public API change, bump the floor first (see "Bumping the floor" below) and the next release becomes `MAJOR.<new-floor-minor>.0`.

Branch the release from dev tip and push it:

```
git fetch origin
git checkout -b release/X.Y.Z origin/development
git push -u origin release/X.Y.Z
```

Open a PR `release/X.Y.Z -> main`.

### While the release is open

Feature work keeps merging into `development` as normal. Review fixes for the open release go straight to `release/X.Y.Z`. Do not cherry-pick review fixes into dev. They flow back when the release is published.

### Merging and tagging

Merge the release branch into dev first, so review fixes flow back:

```
git checkout development
git pull
git merge --no-ff release/X.Y.Z -m "Merge release/X.Y.Z into development"
git push origin development
```

Then merge into main. Use a real merge commit. Do not squash, do not rebase:

```
git checkout main
git pull
git merge --no-ff release/X.Y.Z -m "Release vX.Y.Z"
git push origin main
```

Tag the release branch tip (not `main` HEAD) and push.

```
git tag vX.Y.Z release/X.Y.Z
git push origin vX.Y.Z
```

The tag push triggers `build-win-amd64`, `build-linux-amd64`, `build-linux-arm64`, `build-mac-amd64` and `build-mac-arm64`, which publish a GitHub release with the platform zips.

Delete the release branch (after tagging):

```
git push origin --delete release/X.Y.Z
git branch -d release/X.Y.Z
```

## Hotfixes

A hotfix follows the same flow as a normal release with two differences. The release branch starts from `main` (the last stable tag) instead of dev tip, so it carries the fix and nothing else. The version is the next patch off the most recent tag, same rule as a normal release.

```
git fetch origin
git checkout -b release/X.Y.Z origin/main
```

Apply the fix on the branch, open the PR into main, then merge into dev first and into main with a merge commit, exactly like a normal release. Tag and push.

If a regular release branch is already open, close it without merging before cutting the hotfix. Only one release branch may exist at a time. Re-cut the regular release from dev tip after the hotfix is released. Its version is the next patch off the hotfix tag.

## Bumping the floor

Only when the public API is changing. Single commit on `development`:

1. Edit the `VERSION` line of the `project()` call in `CMakeLists.txt`. Change `0.<old>.0` to `0.<new>.0`.
2. Use the commit message `chore: bump API floor to 0.<new>`.
3. The next release branch is `release/0.<new>.0` and the tag is `v0.<new>.0`.

## Nightly builds

Scheduled workflow that builds dev tip when its SHA has advanced since the last nightly. Each run produces a prerelease tagged `<NIGHTLY_TAG_PREFIX>YYYYMMDD-<short_sha>` and prunes older ones beyond `NIGHTLY_RETENTION`. The nightly builds Windows and Linux. macOS builds only for a release tag or a manual run.

## When something goes wrong

When CI fails on a tag push, do not delete the tag. Investigate, fix on `development`, cut the next patch release when ready. Do not retag.

When a bad release went out, cut a fix release. Do not delete the bad release.

When conflicts arise merging `release/X.Y.Z` into `development`, resolve in favor of dev for feature changes, in favor of the release branch for release fixes. Do not cherry-pick.

When two release branches are open, close the older one without merging.
