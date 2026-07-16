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


def _parse_scalar_list(value: str) -> list[str]:
    value = value.strip()
    if not value:
        return []
    if value.startswith("[") and value.endswith("]"):
        inner = value[1:-1].strip()
        if not inner:
            return []
        return [_unquote(part.strip()) for part in inner.split(",") if part.strip()]
    return [_unquote(part.strip()) for part in value.split(",") if part.strip()]


def _parse_frontmatter(content: str) -> dict[str, object]:
    match = _FRONTMATTER_RE.match(content.replace("\r\n", "\n"))
    if not match:
        raise ValueError("missing YAML frontmatter delimited by ---")

    fields: dict[str, object] = {}
    required_tools: list[str] = []
    in_requires_tools = False

    for raw_line in match.group(1).split("\n"):
        line = raw_line.strip()
        if not line:
            continue

        if line.startswith("- "):
            if not in_requires_tools:
                raise ValueError("list item outside requires_tools")
            tool = _unquote(line[2:].strip())
            if not tool:
                raise ValueError("empty requires_tools entry")
            required_tools.append(tool)
            continue

        in_requires_tools = False
        if ":" not in line:
            raise ValueError(f"invalid frontmatter line: {raw_line}")
        key, value = line.split(":", 1)
        key = key.strip()
        value = value.strip()

        if key == "requires_tools":
            if not value:
                in_requires_tools = True
                continue
            required_tools.extend(_parse_scalar_list(value))
            continue

        fields[key] = _unquote(value)

    if required_tools:
        fields["requires_tools"] = required_tools

    return fields


def _escape_c_string(value: str) -> str:
    return (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\r", "\\r")
        .replace("\n", "\\n")
        .replace("\t", "\\t")
    )


def _load_skill(skill_path: Path) -> dict[str, object]:
    content = skill_path.read_text(encoding="utf-8")
    fields = _parse_frontmatter(content)

    name = str(fields.get("name", "")).strip()
    description = str(fields.get("description", "")).strip()
    if not name:
        raise ValueError(f"{skill_path}: frontmatter.name is required")
    if not description:
        raise ValueError(f"{skill_path}: frontmatter.description is required")
    if len(content.strip()) <= len(_FRONTMATTER_RE.match(content.replace("\r\n", "\n")).group(0)):
        raise ValueError(f"{skill_path}: skill body is empty")

    required_tools = fields.get("requires_tools", [])
    if not isinstance(required_tools, list):
        raise ValueError(f"{skill_path}: requires_tools must be a list")

    return {
        "name": name,
        "description": description,
        "content": content,
        "required_tools": [str(tool).strip() for tool in required_tools if str(tool).strip()],
        "source": str(skill_path).replace("\\", "/"),
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
            "\tconst char *content;\n"
            "\tconst char *required_tools_csv;\n"
            "};\n\n"
        )

        for skill in skills:
            file.write(f'static const char SOLERS_BUILTIN_SKILL_CONTENT_{_slug(skill["name"])}[] =\n')
            file.write(f'\t\t"{_escape_c_string(str(skill["content"]))}";\n\n')

        file.write("static const SolersBuiltinSkillRecord SOLERS_BUILTIN_SKILLS[] = {\n")
        for skill in skills:
            tools = skill["required_tools"]
            tools_csv = ";".join(str(tool) for tool in tools)
            slug = _slug(skill["name"])
            file.write("\t{\n")
            file.write(f'\t\t"{_escape_c_string(str(skill["name"]))}",\n')
            file.write(f'\t\t"{_escape_c_string(str(skill["description"]))}",\n')
            file.write(f"\t\tSOLERS_BUILTIN_SKILL_CONTENT_{slug},\n")
            file.write(f'\t\t"{_escape_c_string(tools_csv)}",\n')
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
