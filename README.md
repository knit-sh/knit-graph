# knit-graph

A standalone C program that accepts a read-only [Cypher](https://opencypher.org/) statement,
translates it to SQL, and runs it against a SQLite provenance database.

- Design: [specifications.md](specifications.md)
- Milestones: [milestones.md](milestones.md)

## Building

```sh
sudo apt-get install -y libsqlite3-dev   # provides sqlite3.h + libsqlite3
autoreconf -i
./configure
make
```

## Usage

```sh
knit-graph DBFILE 'CYPHER…'
```

More output formats and the full Cypher read subset are added over the course of the milestones.
