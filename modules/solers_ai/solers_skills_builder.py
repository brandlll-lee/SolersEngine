"""Build-time compiler for Solers built-in skills."""

from __future__ import annotations

import re
import sys
from pathlib import Path

import methods

_FRONTMATTER_RE = re.compile(r"^---\s*\n(.*?)\n---\s*\n", re.DOTALL)


def _unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ('"', "'"):
        return value[1:-1]
    return value


def _parse_frontmatter(content: str) -> dict[str, object]:
    match = _FRONTMATTER_RE.match(content.replace("\r\n", "\n"))
    if not match:
        raise ValueError("missing YAML frontmatter delimited by ---")

    fields: dict[str, object] = {}
    for raw_line in match.group(1).split("\n"):
        line = raw_line.strip()
        if not line:
            continue
        if ":" not in line:
            raise ValueError(f"invalid frontmatter line: {raw_line}")
        key, value = line.split(":", 1)
        key = key.strip()
        value = value.strip()
        fields[key] = _unquote(value)

    return fields


def _escape_c_string(value: str) -> str:
    return (
        value.replace("\\", "\\\\").replace('"', '\\"').replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t")
    )


def _load_skill(skill_path: Path) -> dict[str, object]:
    content = skill_path.read_text(encoding="utf-8")
    frontmatter = _FRONTMATTER_RE.match(content.replace("\r\n", "\n"))
    if frontmatter is None:
        raise ValueError(f"{skill_path}: missing YAML frontmatter delimited by ---")
    fields = _parse_frontmatter(content)

    name = str(fields.get("name", "")).strip()
    description = str(fields.get("description", "")).strip()
    if not name:
        raise ValueError(f"{skill_path}: frontmatter.name is required")
    if not description:
        raise ValueError(f"{skill_path}: frontmatter.description is required")
    if len(content.strip()) <= len(frontmatter.group(0)):
        raise ValueError(f"{skill_path}: skill body is empty")

    tools = [item.strip() for item in str(fields.get("tools", "")).split(",") if item.strip()]
    return {
        "name": name,
        "description": description,
        "tools": tools,
        "content": content,
    }


def make_builtin_skills_header(target, source, env):
    del env

    skills = [_load_skill(Path(str(src))) for src in source]
    skills.sort(key=lambda skill: str(skill["name"]))

    seen: set[str] = set()
    for skill in skills:
        name = str(skill["name"])
        if name in seen:
            raise ValueError(f"duplicate built-in skill name: {name}")
        seen.add(name)

    with methods.generated_wrapper(str(target[0])) as file:
        file.write(
            "struct SolersBuiltinSkillRecord {\n"
            "\tconst char *name;\n"
            "\tconst char *description;\n"
            "\tconst char *tools;\n"
            "\tconst char *content;\n"
            "};\n\n"
        )

        for skill in skills:
            file.write(f"static const char SOLERS_BUILTIN_SKILL_CONTENT_{_slug(skill['name'])}[] =\n")
            file.write(f'\t\t"{_escape_c_string(str(skill["content"]))}";\n\n')

        file.write("static const SolersBuiltinSkillRecord SOLERS_BUILTIN_SKILLS[] = {\n")
        for skill in skills:
            slug = _slug(skill["name"])
            file.write("\t{\n")
            file.write(f'\t\t"{_escape_c_string(str(skill["name"]))}",\n')
            file.write(f'\t\t"{_escape_c_string(str(skill["description"]))}",\n')
            file.write(f'\t\t"{_escape_c_string(",".join(skill["tools"]))}",\n')
            file.write(f"\t\tSOLERS_BUILTIN_SKILL_CONTENT_{slug},\n")
            file.write("\t},\n")
        file.write("};\n\n")
        file.write(f"static const int SOLERS_BUILTIN_SKILL_COUNT = {len(skills)};\n")


def _slug(name: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_").upper()
    if not slug:
        raise ValueError(f"invalid skill name: {name}")
    return slug


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: solers_skills_builder.py <output.gen.h>", file=sys.stderr)
        sys.exit(1)
    skills_root = Path(__file__).resolve().parent / "skills"
    sources = sorted(skills_root.glob("*/SKILL.md"))
    if not sources:
        print(f"no built-in skills found under {skills_root}", file=sys.stderr)
        sys.exit(1)
    make_builtin_skills_header([sys.argv[1]], sources, None)
