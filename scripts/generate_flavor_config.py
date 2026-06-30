#!/usr/bin/env python3
"""Generate SolarOS package configuration from a flavor TOML file."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import sys
import tomllib


DEFAULT_PACKAGE_CATALOG = Path(__file__).resolve().parents[1] / "packages" / "solar_os_packages.toml"


@dataclass(frozen=True)
class PackageDef:
    label: str
    sources: tuple[str, ...]
    requires: tuple[str, ...]
    capabilities: tuple[str, ...]


@dataclass(frozen=True)
class GroupDef:
    members: tuple[str, ...]
    triggers: tuple[str, ...]
    sources: tuple[str, ...]
    requires: tuple[str, ...]
    capabilities: tuple[str, ...]


@dataclass(frozen=True)
class PackageCatalog:
    groups: tuple[str, ...]
    packages: tuple[str, ...]
    group_defs: dict[str, GroupDef]
    package_defs: dict[str, PackageDef]


def c_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def cmake_string(value: str | Path) -> str:
    text = str(value)
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace(";", "\\;") + '"'


def cmake_list(name: str, values: list[str]) -> str:
    if not values:
        return f"set({name})"
    lines = [f"set({name}"]
    lines.extend(f"    {cmake_string(value)}" for value in values)
    lines.append(")")
    return "\n".join(lines)


def write_if_changed(path: Path, content: str) -> None:
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def string_tuple(value: object, key: str) -> tuple[str, ...]:
    if value is None:
        return ()
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise ValueError(f"{key} must be a list of strings")
    return tuple(value)


def package_macro(name: str) -> str:
    return f"SOLAR_OS_PACKAGE_{name.upper()}"


def default_package_label(name: str) -> str:
    prefix, _, rest = name.partition("_")
    return f"{prefix}.{rest.replace('_', '-')}"


def unique(values: list[str]) -> list[str]:
    seen: set[str] = set()
    result: list[str] = []
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        result.append(value)
    return result


def load_catalog(path: Path) -> PackageCatalog:
    with path.open("rb") as file:
        data = tomllib.load(file)

    raw_groups = data.get("groups", {})
    raw_packages = data.get("packages", {})
    if not isinstance(raw_groups, dict) or not raw_groups:
        raise ValueError("package catalog has no [groups]")
    if not isinstance(raw_packages, dict) or not raw_packages:
        raise ValueError("package catalog has no [packages]")

    packages = tuple(raw_packages.keys())
    package_set = set(packages)
    package_defs: dict[str, PackageDef] = {}
    for name, raw in raw_packages.items():
        if not isinstance(raw, dict):
            raise ValueError(f"packages.{name} must be a table")
        package_defs[name] = PackageDef(
            label=str(raw.get("label") or default_package_label(name)),
            sources=string_tuple(raw.get("sources"), f"packages.{name}.sources"),
            requires=string_tuple(raw.get("requires"), f"packages.{name}.requires"),
            capabilities=string_tuple(raw.get("capabilities"), f"packages.{name}.capabilities"),
        )

    groups = tuple(raw_groups.keys())
    group_defs: dict[str, GroupDef] = {}
    for name, raw in raw_groups.items():
        if not isinstance(raw, dict):
            raise ValueError(f"groups.{name} must be a table")
        members = string_tuple(raw.get("members"), f"groups.{name}.members")
        unknown_members = sorted(set(members) - package_set)
        if unknown_members:
            raise ValueError(f"groups.{name} has unknown member(s): {', '.join(unknown_members)}")
        triggers = string_tuple(raw.get("triggers"), f"groups.{name}.triggers")
        unknown_triggers = sorted(set(triggers) - package_set)
        if unknown_triggers:
            raise ValueError(f"groups.{name} has unknown trigger(s): {', '.join(unknown_triggers)}")
        group_defs[name] = GroupDef(
            members=members,
            triggers=triggers,
            sources=string_tuple(raw.get("sources"), f"groups.{name}.sources"),
            requires=string_tuple(raw.get("requires"), f"groups.{name}.requires"),
            capabilities=string_tuple(raw.get("capabilities"), f"groups.{name}.capabilities"),
        )

    if "core" not in group_defs:
        raise ValueError("package catalog must define groups.core")

    return PackageCatalog(
        groups=groups,
        packages=packages,
        group_defs=group_defs,
        package_defs=package_defs,
    )


def load_flavor(path: Path,
                catalog: PackageCatalog) -> tuple[str, str, dict[str, bool], dict[str, bool]]:
    with path.open("rb") as file:
        data = tomllib.load(file)

    flavor = data.get("flavor", {})
    package_groups = data.get("package_groups", {})
    packages = data.get("packages", {})
    name = str(flavor.get("name") or path.stem)
    description = str(flavor.get("description") or "")

    groups_enabled = {group: False for group in catalog.groups}
    packages_enabled = {package: False for package in catalog.packages}
    package_overrides: dict[str, bool] = {}

    unknown_groups = sorted(set(package_groups) - set(catalog.groups))
    if unknown_groups:
        raise ValueError(f"unknown package group key(s): {', '.join(unknown_groups)}")
    for group, value in package_groups.items():
        groups_enabled[group] = bool(value)

    unknown_packages: list[str] = []
    for key, value in packages.items():
        if key in catalog.group_defs:
            groups_enabled[key] = bool(value)
        elif key in catalog.package_defs:
            package_overrides[key] = bool(value)
        else:
            unknown_packages.append(key)
    if unknown_packages:
        raise ValueError(f"unknown package key(s): {', '.join(sorted(unknown_packages))}")

    groups_enabled["core"] = True
    if groups_enabled.get("python", False) or package_overrides.get("app_python", False):
        groups_enabled["net"] = True

    for group, group_def in catalog.group_defs.items():
        if groups_enabled[group]:
            for member in group_def.members:
                packages_enabled[member] = True

    for package, value in package_overrides.items():
        packages_enabled[package] = value

    for member in catalog.group_defs["core"].members:
        packages_enabled[member] = True

    groups_effective = dict(groups_enabled)
    groups_effective["core"] = True
    for group, group_def in catalog.group_defs.items():
        if any(packages_enabled[package] for package in group_def.triggers):
            groups_effective[group] = True

    return name, description, groups_effective, packages_enabled


def collect_sources(catalog: PackageCatalog,
                    groups_enabled: dict[str, bool],
                    packages_enabled: dict[str, bool]) -> list[str]:
    sources: list[str] = []
    for group in catalog.groups:
        if groups_enabled[group]:
            sources.extend(catalog.group_defs[group].sources)
    for package in catalog.packages:
        if packages_enabled[package]:
            sources.extend(catalog.package_defs[package].sources)
    return unique(sources)


def collect_requires(catalog: PackageCatalog,
                     groups_enabled: dict[str, bool],
                     packages_enabled: dict[str, bool]) -> list[str]:
    requires: list[str] = []
    for group in catalog.groups:
        if groups_enabled[group]:
            requires.extend(catalog.group_defs[group].requires)
    for package in catalog.packages:
        if packages_enabled[package]:
            requires.extend(catalog.package_defs[package].requires)
    return unique(requires)


def collect_capabilities(catalog: PackageCatalog,
                         groups_enabled: dict[str, bool],
                         packages_enabled: dict[str, bool]) -> list[str]:
    capabilities: list[str] = []
    for group in catalog.groups:
        if groups_enabled[group]:
            capabilities.extend(catalog.group_defs[group].capabilities)
    for package in catalog.packages:
        if packages_enabled[package]:
            capabilities.extend(catalog.package_defs[package].capabilities)
    return unique(capabilities)


def collect_job_packages(catalog: PackageCatalog,
                         packages_enabled: dict[str, bool]) -> list[str]:
    return [
        package
        for package in catalog.packages
        if packages_enabled[package] and package.startswith("job_")
    ]


def generate_header(name: str,
                    description: str,
                    groups_enabled: dict[str, bool],
                    packages_enabled: dict[str, bool],
                    catalog: PackageCatalog,
                    source: Path,
                    package_catalog: Path) -> str:
    enabled_groups = [group for group in catalog.groups if groups_enabled[group]]
    enabled_packages = [package for package in catalog.packages if packages_enabled[package]]
    enabled_package_labels = [catalog.package_defs[package].label for package in enabled_packages]
    enabled_job_count = len(collect_job_packages(catalog, packages_enabled))
    lines = [
        "/* Generated by scripts/generate_flavor_config.py. Do not edit. */",
        "#pragma once",
        "",
        f"#define SOLAR_OS_FLAVOR_NAME {c_string(name)}",
        f"#define SOLAR_OS_FLAVOR_DESCRIPTION {c_string(description)}",
        f"#define SOLAR_OS_FLAVOR_FILE {c_string(str(source))}",
        f"#define SOLAR_OS_PACKAGE_CATALOG_FILE {c_string(str(package_catalog))}",
        f"#define SOLAR_OS_PACKAGE_GROUP_LIST {c_string(' '.join(enabled_groups))}",
        f"#define SOLAR_OS_PACKAGE_LIST {c_string(' '.join(enabled_package_labels))}",
        f"#define SOLAR_OS_PACKAGE_REQUIRED_CAPABILITIES {c_string(' '.join(collect_capabilities(catalog, groups_enabled, packages_enabled)))}",
        f"#define SOLAR_OS_JOBS_MAX {enabled_job_count}",
        "",
    ]
    for group in catalog.groups:
        lines.append(f"#define {package_macro(group)} {1 if groups_enabled[group] else 0}")
    lines.append("")
    for package in catalog.packages:
        lines.append(f"#define {package_macro(package)} {1 if packages_enabled[package] else 0}")
    lines.append("")
    return "\n".join(lines)


def generate_cmake(name: str,
                   description: str,
                   groups_enabled: dict[str, bool],
                   packages_enabled: dict[str, bool],
                   catalog: PackageCatalog,
                   source: Path,
                   package_catalog: Path) -> str:
    enabled_groups = [group for group in catalog.groups if groups_enabled[group]]
    enabled_package_labels = [
        catalog.package_defs[package].label
        for package in catalog.packages
        if packages_enabled[package]
    ]
    enabled_job_count = len(collect_job_packages(catalog, packages_enabled))
    lines = [
        "# Generated by scripts/generate_flavor_config.py. Do not edit.",
        f"set(SOLAR_OS_FLAVOR_NAME {cmake_string(name)})",
        f"set(SOLAR_OS_FLAVOR_DESCRIPTION {cmake_string(description)})",
        f"set(SOLAR_OS_FLAVOR_FILE {cmake_string(source)})",
        f"set(SOLAR_OS_PACKAGE_CATALOG_FILE {cmake_string(package_catalog)})",
        f"set(SOLAR_OS_PACKAGE_GROUP_LIST {cmake_string(' '.join(enabled_groups))})",
        f"set(SOLAR_OS_PACKAGE_LIST {cmake_string(' '.join(enabled_package_labels))})",
        f"set(SOLAR_OS_PACKAGE_REQUIRED_CAPABILITIES {cmake_string(' '.join(collect_capabilities(catalog, groups_enabled, packages_enabled)))})",
        f"set(SOLAR_OS_JOBS_MAX {enabled_job_count})",
    ]
    for group in catalog.groups:
        lines.append(f"set({package_macro(group)} {1 if groups_enabled[group] else 0})")
    lines.append("")
    for package in catalog.packages:
        lines.append(f"set({package_macro(package)} {1 if packages_enabled[package] else 0})")
    lines.extend([
        "",
        cmake_list("SOLAR_OS_PACKAGE_SRCS",
                   collect_sources(catalog, groups_enabled, packages_enabled)),
        cmake_list("SOLAR_OS_PACKAGE_REQUIRES",
                   collect_requires(catalog, groups_enabled, packages_enabled)),
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--packages", default=DEFAULT_PACKAGE_CATALOG, type=Path)
    parser.add_argument("--header", required=True, type=Path)
    parser.add_argument("--cmake", required=True, type=Path)
    args = parser.parse_args()

    try:
        catalog = load_catalog(args.packages)
        name, description, groups_enabled, packages_enabled = load_flavor(args.input, catalog)
        write_if_changed(args.header,
                         generate_header(name,
                                         description,
                                         groups_enabled,
                                         packages_enabled,
                                         catalog,
                                         args.input,
                                         args.packages))
        write_if_changed(args.cmake,
                         generate_cmake(name,
                                        description,
                                        groups_enabled,
                                        packages_enabled,
                                        catalog,
                                        args.input,
                                        args.packages))
    except Exception as exc:
        print(f"generate_flavor_config.py: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
