# How to contribute

The NEST simulator is a scientific tool and as such it is never ready and constantly changing to meet the needs of novel neuroscientific endeavors. Here you find the the most important information on how you can contribute to NEST. This document is an excerpt from our [developer space](https://nest.github.io/nest-simulator/), which provides more detailed information.

## Getting started

* Make sure you have a [GitHub account](https://github.com/signup/free)
* The development workflow is based purely on pull requests. This [article](https://nest.github.io/nest-simulator/development_workflow) gives information on how to work with git and GitHub, if you are new to it.

## Making Changes

* Create a topic branch from NEST master in your fork. Please avoid working directly on the `master` branch.
* Make commits of logical units.
* Make sure NEST compiles and has no new warnings.
* Make sure all tests pass (`make installcheck`).
* Make sure your code conforms to our [coding guidelines](https://nest.github.io/nest-simulator/coding_guidelines_c++#check-static-analysis-local) (`./extras/check_code_style.sh`)


## Code Review

We review each pull request according to our [code review guidelines](https://nest.github.io/nest-simulator/code_review_guidelines):

* In general, the rule is that each pull request needs an OK from the CI platform and at least two reviewers to be merged.
* For changes labeled “not code” or “minor” (e.g. changes in documentation, fixes for typos, etc.), the release manager can waive the need for code review and just accept the OK from Travis in order to merge the request.
* Each pull request needs to be documented by an issue in the [issue tracker](https://github.com/nest/nest-simulator/issues) explaining the reason for the changes and the solution. The issue is also the place for discussions about the code.
* New features like SLI or PyNEST functions, neuron or synapse models need to be accompanied by one or more tests written either in SLI or Python. New features for the NEST kernel need a test written in SLI.
* Each change to the code has to be reflected also in the corresponding examples and documentation.
* All source code has to be adhering to the Coding Guidelines for [C++](https://nest.github.io/nest-simulator/coding_guidelines_c++) and [SLI](https://nest.github.io/nest-simulator/coding_guidelines_sli) in order to pass the continuous integration system checks.
* All Commits should be coherent and contain only changes that belong together.

## Submitting Changes

* Sign the [Contributor License Agreement](https://nest.github.io/nest-simulator/#contributor-license-agreement).
* Push your changes to a topic branch in your fork of the repository.
* Submit a pull request to the [NEST repository](https://github.com/nest/nest-simulator).
* The core team looks at Pull Requests on a regular basis and posts feedback.
* After feedback has been given we expect responses within two weeks. After two weeks we may close the pull request if it isn't showing any activity.

# Additional Resources

* The [NEST Developer Space](https://nest.github.io/nest-simulator/).
    * Writing [Extension Modules](https://nest.github.io/nest-simulator/extension_modules).
    * Writing [Neuron and Device Models](https://nest.github.io/nest-simulator/neuron_and_device_models).
    * Writing [Synapse Models](https://nest.github.io/nest-simulator/synapse_models).
    * [Updating Models](https://nest.github.io/nest-simulator/model_conversion_3g_4g).
    * [Dig Deeper](https://nest.github.io/nest-simulator/#dig-deeper) into NEST.
* The [NEST Simulator homepage](http://nest-simulator.org/).
* The [NEST Initiative homepage](http://www.nest-initiative.org/).
