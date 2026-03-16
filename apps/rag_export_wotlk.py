#!/usr/bin/env python3

import argparse
import json
import re
import subprocess
from pathlib import Path
from typing import Dict, List, Sequence, Tuple


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export WotLK-focused RAG JSON files from AzerothCore world database."
    )
    parser.add_argument(
        "--worldserver-conf",
        default="/home/cachyd/azeroth-server/etc/worldserver.conf",
        help="Path to worldserver.conf containing WorldDatabaseInfo",
    )
    parser.add_argument(
        "--output-dir",
        default="/home/cachyd/acore/modules/mod-ollama-chat/data/rag",
        help="Directory where JSON files will be generated",
    )
    parser.add_argument(
        "--mysql-bin",
        default="mysql",
        help="MySQL CLI binary path",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=500,
        help="Max entries per dataset (default: 500)",
    )
    parser.add_argument(
        "--patch",
        default="3.3.5a",
        help="Patch tag to include in generated entries",
    )
    parser.add_argument(
        "--expansion",
        default="wotlk",
        help="Expansion tag to include in generated entries",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print summary only, do not write files",
    )
    return parser.parse_args()


def parse_world_db_info(conf_path: Path) -> Dict[str, str]:
    text = conf_path.read_text(encoding="utf-8", errors="ignore")
    match = re.search(r'^\s*WorldDatabaseInfo\s*=\s*"([^"]+)"', text, flags=re.MULTILINE)
    if not match:
        raise ValueError(f"WorldDatabaseInfo not found in {conf_path}")

    parts = match.group(1).split(";")
    if len(parts) < 5:
        raise ValueError(
            f"Invalid WorldDatabaseInfo format in {conf_path}: expected host;port;user;pass;database"
        )

    return {
        "host": parts[0],
        "port": parts[1],
        "user": parts[2],
        "password": parts[3],
        "database": parts[4],
    }


def run_mysql_query(mysql_bin: str, db: Dict[str, str], query: str) -> List[List[str]]:
    cmd = [
        mysql_bin,
        f"--host={db['host']}",
        f"--port={db['port']}",
        f"--user={db['user']}",
        f"--password={db['password']}",
        f"--database={db['database']}",
        "--batch",
        "--raw",
        "--skip-column-names",
        "--default-character-set=utf8mb4",
        "--execute",
        query,
    ]

    try:
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as exc:
        stderr = exc.stderr.strip() if exc.stderr else ""
        raise RuntimeError(f"MySQL query failed: {stderr}\nQuery: {query}") from exc

    output = result.stdout.strip()
    if not output:
        return []

    rows: List[List[str]] = []
    for line in output.splitlines():
        rows.append(line.split("\t"))
    return rows


def get_table_columns(mysql_bin: str, db: Dict[str, str], table: str) -> List[str]:
    query = (
        "SELECT COLUMN_NAME "
        "FROM INFORMATION_SCHEMA.COLUMNS "
        f"WHERE TABLE_SCHEMA = '{db['database']}' AND TABLE_NAME = '{table}' "
        "ORDER BY ORDINAL_POSITION"
    )
    rows = run_mysql_query(mysql_bin, db, query)
    return [row[0] for row in rows]


def pick_columns(available: Sequence[str], preferred: Sequence[str]) -> List[str]:
    available_set = set(available)
    return [column for column in preferred if column in available_set]


def slugify(value: str) -> str:
    value = value.lower()
    value = re.sub(r"[^a-z0-9]+", "_", value)
    value = value.strip("_")
    return value or "unknown"


def compact(value: str, limit: int = 320) -> str:
    value = " ".join((value or "").split())
    return value[:limit].rstrip()


def keywordize(*parts: str) -> List[str]:
    keywords: List[str] = []
    seen = set()

    for part in parts:
        text = (part or "").lower()
        text = re.sub(r"[^a-z0-9\s]", " ", text)
        for token in text.split():
            if len(token) < 3:
                continue
            if token in seen:
                continue
            seen.add(token)
            keywords.append(token)
            if len(keywords) >= 24:
                return keywords

    return keywords


def build_entries_for_quests(
    rows: List[List[str]], selected_columns: List[str], expansion: str, patch: str
) -> List[Dict[str, object]]:
    entries: List[Dict[str, object]] = []

    for row in rows:
        values = dict(zip(selected_columns, row))
        quest_id = values.get("ID", "")
        title = compact(values.get("LogTitle", ""))
        if not quest_id or not title:
            continue

        level = values.get("QuestLevel", "")
        min_level = values.get("MinLevel", "")
        zone_or_sort = values.get("ZoneOrSort", "")

        content = compact(
            f"Quest '{title}' (ID {quest_id}) has suggested level {level or 'n/a'} and minimum level "
            f"{min_level or 'n/a'}. ZoneOrSort value: {zone_or_sort or 'n/a'}."
        )

        entries.append(
            {
                "id": f"quest_{quest_id}",
                "title": f"Quest: {title}",
                "content": content,
                "keywords": keywordize(title, f"quest {quest_id}", f"level {level}", f"minimum {min_level}"),
                "tags": ["quest", expansion, f"patch_{patch}", "db_export"],
            }
        )

    return entries


def build_entries_for_items(
    rows: List[List[str]], selected_columns: List[str], expansion: str, patch: str
) -> List[Dict[str, object]]:
    entries: List[Dict[str, object]] = []

    for row in rows:
        values = dict(zip(selected_columns, row))
        item_id = values.get("entry", "")
        name = compact(values.get("name", ""))
        if not item_id or not name:
            continue

        quality = values.get("Quality", "")
        item_level = values.get("ItemLevel", "")
        req_level = values.get("RequiredLevel", "")
        klass = values.get("class", "")
        subclass = values.get("subclass", "")
        inv_type = values.get("InventoryType", "")

        content = compact(
            f"Item '{name}' (ID {item_id}) has quality {quality or 'n/a'}, item level {item_level or 'n/a'}, "
            f"required level {req_level or 'n/a'}, class {klass or 'n/a'}, subclass {subclass or 'n/a'}, "
            f"inventory type {inv_type or 'n/a'}."
        )

        entries.append(
            {
                "id": f"item_{item_id}",
                "title": f"Item: {name}",
                "content": content,
                "keywords": keywordize(name, f"item {item_id}", f"ilevel {item_level}", f"required level {req_level}"),
                "tags": ["item", expansion, f"patch_{patch}", "db_export"],
            }
        )

    return entries


def build_entries_for_creatures(
    rows: List[List[str]], selected_columns: List[str], expansion: str, patch: str
) -> List[Dict[str, object]]:
    entries: List[Dict[str, object]] = []

    for row in rows:
        values = dict(zip(selected_columns, row))
        creature_id = values.get("entry", "")
        name = compact(values.get("name", ""))
        if not creature_id or not name:
            continue

        min_level = values.get("minlevel", "")
        max_level = values.get("maxlevel", "")
        rank = values.get("rank", "")
        faction = values.get("faction", "")

        content = compact(
            f"Creature '{name}' (entry {creature_id}) has level range {min_level or 'n/a'}-{max_level or 'n/a'}, "
            f"rank {rank or 'n/a'}, and faction template {faction or 'n/a'}."
        )

        entries.append(
            {
                "id": f"creature_{creature_id}",
                "title": f"Creature: {name}",
                "content": content,
                "keywords": keywordize(name, f"npc {creature_id}", f"mob {creature_id}", f"level {min_level} {max_level}"),
                "tags": ["npc", expansion, f"patch_{patch}", "db_export"],
            }
        )

    return entries


def build_entries_for_gameobjects(
    rows: List[List[str]], selected_columns: List[str], expansion: str, patch: str
) -> List[Dict[str, object]]:
    entries: List[Dict[str, object]] = []

    for row in rows:
        values = dict(zip(selected_columns, row))
        go_id = values.get("entry", "")
        name = compact(values.get("name", ""))
        if not go_id or not name:
            continue

        go_type = values.get("type", "")
        display_id = values.get("displayId", "")

        content = compact(
            f"GameObject '{name}' (entry {go_id}) has type {go_type or 'n/a'} and displayId {display_id or 'n/a'}."
        )

        entries.append(
            {
                "id": f"gameobject_{go_id}",
                "title": f"GameObject: {name}",
                "content": content,
                "keywords": keywordize(name, f"gameobject {go_id}", f"object {go_id}", f"type {go_type}"),
                "tags": ["gameobject", expansion, f"patch_{patch}", "db_export"],
            }
        )

    return entries


def write_json_file(path: Path, data: List[Dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def build_dataset(
    mysql_bin: str,
    db: Dict[str, str],
    table: str,
    preferred_columns: Sequence[str],
    order_column: str,
    limit: int,
) -> Tuple[List[List[str]], List[str]]:
    available_columns = get_table_columns(mysql_bin, db, table)
    selected_columns = pick_columns(available_columns, preferred_columns)
    if len(selected_columns) < 2:
        raise RuntimeError(
            f"Not enough compatible columns found for table '{table}'. Available: {available_columns}"
        )

    query = (
        f"SELECT {', '.join(selected_columns)} FROM {table} "
        f"WHERE {selected_columns[1]} IS NOT NULL AND {selected_columns[1]} <> '' "
        f"ORDER BY {order_column} LIMIT {limit}"
    )
    rows = run_mysql_query(mysql_bin, db, query)
    return rows, selected_columns


def main() -> int:
    args = parse_args()
    conf_path = Path(args.worldserver_conf)
    output_dir = Path(args.output_dir)

    if not conf_path.exists():
        raise FileNotFoundError(f"worldserver.conf not found: {conf_path}")

    db = parse_world_db_info(conf_path)

    datasets = [
        {
            "table": "quest_template",
            "preferred_columns": ["ID", "LogTitle", "QuestLevel", "MinLevel", "ZoneOrSort"],
            "order_column": "ID",
            "filename": "wow_wotlk_quests_db.json",
            "builder": build_entries_for_quests,
        },
        {
            "table": "item_template",
            "preferred_columns": [
                "entry",
                "name",
                "Quality",
                "ItemLevel",
                "RequiredLevel",
                "class",
                "subclass",
                "InventoryType",
            ],
            "order_column": "entry",
            "filename": "wow_wotlk_items_db.json",
            "builder": build_entries_for_items,
        },
        {
            "table": "creature_template",
            "preferred_columns": ["entry", "name", "minlevel", "maxlevel", "rank", "faction"],
            "order_column": "entry",
            "filename": "wow_wotlk_creatures_db.json",
            "builder": build_entries_for_creatures,
        },
        {
            "table": "gameobject_template",
            "preferred_columns": ["entry", "name", "type", "displayId"],
            "order_column": "entry",
            "filename": "wow_wotlk_gameobjects_db.json",
            "builder": build_entries_for_gameobjects,
        },
    ]

    summary: List[Tuple[str, int]] = []

    for dataset in datasets:
        rows, selected_columns = build_dataset(
            mysql_bin=args.mysql_bin,
            db=db,
            table=dataset["table"],
            preferred_columns=dataset["preferred_columns"],
            order_column=dataset["order_column"],
            limit=args.limit,
        )

        entries = dataset["builder"](rows, selected_columns, args.expansion, args.patch)

        output_file = output_dir / dataset["filename"]
        if not args.dry_run:
            write_json_file(output_file, entries)

        summary.append((dataset["filename"], len(entries)))

    print("WotLK RAG export summary")
    print(f"- world db: {db['database']}@{db['host']}:{db['port']}")
    print(f"- output dir: {output_dir}")
    for filename, count in summary:
        print(f"- {filename}: {count} entries")

    if args.dry_run:
        print("Dry run complete (no files written).")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
