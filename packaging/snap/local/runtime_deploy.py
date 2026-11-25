# SPDX-License-Identifier: MPL-2.0
"""Custom conan install deployer for Linux shared libraries and license notices."""

import shutil
from pathlib import Path

from conan.tools.files import copy


def deploy(graph, output_folder, **kwargs):
    """Deploy into a snap payload root, omitting dependency executables and SDK files."""
    root = Path(output_folder)
    libraries = root / "usr/lib"
    licenses = root / "usr/share/doc/plotjuggler/third-party"
    for requirement, dependency in graph.root.conanfile.dependencies.host.items():
        if not dependency.package_folder:
            continue
        if requirement.run:
            for directory in dependency.cpp_info.aggregated_components().libdirs:
                copy(
                    graph.root.conanfile,
                    "*.so*",
                    src=directory,
                    dst=str(libraries),
                    keep_path=True,
                )
        source = Path(dependency.package_folder) / "licenses"
        if source.is_dir():
            name = f"{dependency.ref.name}-{dependency.ref.version}"
            shutil.copytree(source, licenses / name, dirs_exist_ok=True)
