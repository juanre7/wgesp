#!/usr/bin/env python3
"""Avisa cuando una dependencia del ESP Component Registry se queda atras.

No hay un deps.rs para ESP-IDF: deps.rs solo lee Cargo.toml y Dependabot no
soporta idf_component.yml. Pero el registro tiene API publica, asi que aqui
leemos cada idf_component.yml, preguntamos por la ultima version publicada y
fallamos si el rango declarado ya no la cubre (una mayor nueva, o una menor
nueva cuando la mayor es 0).

Uso: python3 scripts/check_deps.py [raiz-del-repo]
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


def main(raiz):
    fallo = 0
    for manifiesto in sorted(pathlib.Path(raiz).glob("**/idf_component.yml")):
        for nombre, rango in dependencias(manifiesto):
            version = ultima(nombre)
            v = ".".join(str(n) for n in version)
            donde = manifiesto.relative_to(raiz)
            estado = cubre(rango, version)
            if estado is None:
                print(f"?  {nombre} {rango} ({donde}): rango no reconocido, ultima {v}")
                fallo = 1
            elif estado:
                print(f"OK {nombre} {rango} ({donde}): ultima {v}")
            else:
                print(f"::warning file={donde}::{nombre} {rango} se ha quedado atras: ya hay {v}")
                fallo = 1
    return fallo


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
