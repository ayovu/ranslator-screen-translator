# Repository Guidelines

## Project Structure & Module Organization

This directory is currently empty: no source code, tests, assets, or package manifests are present. Establish the layout with the first implementation and document it in `README.md`. Unless the selected framework requires another structure, use `src/` for application code, `tests/` for automated tests, and `assets/` for static resources. Group related functionality into focused modules.

## Build, Test, and Development Commands

No build, test, or development commands are configured yet. When introducing tooling, provide reproducible commands for installing dependencies, running locally, building, and testing. Record the exact commands in `README.md` and commit the relevant manifests and lockfiles. Do not assume commands such as `npm test` or `make build` work until their configuration exists.

## Coding Style & Naming Conventions

No language, formatter, or linter has been selected. Follow the chosen language’s standard conventions and configure formatting and linting alongside the initial implementation. Keep indentation consistent within each file. Use descriptive module, function, and variable names; follow framework conventions for filenames. Separate formatting-only changes from behavioral changes when practical.

## Testing Guidelines

No test framework or coverage threshold is established. Add an appropriate framework when executable code is introduced. Cover new behavior and include regression tests for bug fixes. Name tests after the behavior they verify and follow the selected framework’s discovery rules. Document how to run the full suite and individual tests.

## Commit & Pull Request Guidelines

This directory is not yet a Git repository, so no commit conventions can be inferred. Once version control is initialized, use concise, imperative commit subjects, such as `Add initial application scaffold`. Keep commits focused. Pull requests should describe the change, explain its purpose, link relevant issues, and report validation performed. Include screenshots for visible interface changes.

## Security & Configuration

Keep credentials and local configuration out of version control. Provide placeholder values in an example configuration file and document required settings. Add ignore rules for generated output and local dependency directories when tooling is introduced.
