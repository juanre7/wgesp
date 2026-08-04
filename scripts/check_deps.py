#!/usr/bin/env python3
"""Avisa cuando una dependencia del ESP Component Registry se queda atras.

No hay un deps.rs para ESP-IDF: deps.rs solo lee Cargo.toml y Dependabot no
soporta idf_component.yml. Pero el registro tiene API publica, asi que aqui
leemos cada idf_component.yml, preguntamos por la ultima version publicada y
fallamos si el rango declarado ya no la cubre (una mayor nueva, o una menor
nueva cuando la mayor es 0).

Uso: python3 scripts/check_deps.py [raiz-del-repo] [--badge fichero.json]
"""

import json
import pathlib
import re
import sys
import urllib.request

API = "https://components.espressif.com/api/components/{}"
# "^1.0.20", "~1.4", "1.0.20", ">=1.4" ... solo nos interesa el prefijo y los numeros
RANGO = re.compile(r"^\s*([\^~]?)\s*(\d+)(?:\.(\d+))?(?:\.(\d+))?\s*$")
# el registro publica tambien prereleases; para "esta al dia" solo valen las finales
FINAL = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def dependencias(manifiesto):
    """Pares (nombre, rango) de un idf_component.yml, sin dependencia de PyYAML.

    Los manifiestos son planos: una clave 'dependencies:' y debajo
    'namespace/nombre: "rango"'. Cualquier otra forma (git:, override_path:,
    version: anidado) se ignora a proposito: no vive en el registro o no
    tiene una version que comparar.
    """
    dentro = False
    for linea in manifiesto.read_text().splitlines():
        if not linea.strip() or linea.lstrip().startswith("#"):
            continue
        if not linea[0].isspace():
            dentro = linea.strip() == "dependencies:"
            continue
        if not dentro:
            continue
        clave, _, valor = linea.partition(":")
        nombre, rango = clave.strip(), valor.strip().strip('"').strip("'")
        if "/" in nombre and rango:
            yield nombre, rango


def ultima(nombre):
    with urllib.request.urlopen(API.format(nombre), timeout=30) as r:
        datos = json.load(r)
    versiones = [
        tuple(int(g) for g in m.groups())
        for v in datos["versions"]
        if (m := FINAL.match(v["version"]))
    ]
    if not versiones:
        raise SystemExit(f"{nombre}: el registro no devuelve ninguna version final")
    return max(versiones)


def cubre(rango, version):
    """Si el rango declarado admite esa version. Semantica de Cargo/npm."""
    m = RANGO.match(rango)
    if not m:
        return None  # rango raro: lo reportamos como no comprobable
    prefijo, mayor, menor, parche = m.group(1), *(int(g or 0) for g in m.groups()[1:])
    minimo = (mayor, menor, parche)
    if prefijo == "^":
        tope = (mayor + 1, 0, 0) if mayor else (0, menor + 1, 0)
    elif prefijo == "~":
        tope = (mayor, menor + 1, 0)
    else:
        tope = (mayor, menor, parche + 1)
    return minimo <= version < tope


def badge(total, atrasadas):
    """JSON para el badge 'endpoint' de shields, con el recuento a la vista."""
    if atrasadas:
        mensaje, color = f"{atrasadas} of {total} outdated", "orange"
    else:
        mensaje, color = f"{total} up to date", "brightgreen"
    return json.dumps(
        {"schemaVersion": 1, "label": "deps", "message": mensaje, "color": color},
        indent=2,
    ) + "\n"


def main(raiz, destino_badge=None):
    total = atrasadas = 0
    for manifiesto in sorted(pathlib.Path(raiz).glob("**/idf_component.yml")):
        for nombre, rango in dependencias(manifiesto):
            version = ultima(nombre)
            v = ".".join(str(n) for n in version)
            donde = manifiesto.relative_to(raiz)
            estado = cubre(rango, version)
            total += 1
            if estado is None:
                print(f"?  {nombre} {rango} ({donde}): rango no reconocido, ultima {v}")
                atrasadas += 1
            elif estado:
                print(f"OK {nombre} {rango} ({donde}): ultima {v}")
            else:
                print(f"::warning file={donde}::{nombre} {rango} se ha quedado atras: ya hay {v}")
                atrasadas += 1
    if destino_badge:
        pathlib.Path(destino_badge).write_text(badge(total, atrasadas))
    return 1 if atrasadas else 0


if __name__ == "__main__":
    args = sys.argv[1:]
    destino = None
    if "--badge" in args:
        i = args.index("--badge")
        destino = args[i + 1]
        del args[i : i + 2]
    sys.exit(main(args[0] if args else ".", destino))
