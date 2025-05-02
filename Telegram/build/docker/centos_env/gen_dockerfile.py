#!/usr/bin/env python3
from os import environ
from os.path import dirname, join
from subprocess import run
from jinja2 import Environment, FileSystemLoader

def checkEnv(envName, defaultValue):
    if isinstance(defaultValue, bool):
        return bool(len(environ[envName])) if envName in environ else defaultValue
    return environ[envName] if envName in environ else defaultValue

def main():
    print(Environment(loader=FileSystemLoader(dirname(__file__))).get_template("Dockerfile").render(
        DEBUG=checkEnv("DEBUG", True),
        LTO=checkEnv("LTO", True),
        ASAN=checkEnv("ASAN", False),
        JOBS=checkEnv("JOBS", ""),
        PATCHES=run(["git", "rev-parse", "HEAD"], capture_output=True, cwd=join(dirname(dirname(dirname(__file__))), "patches"), text=True).stdout.strip(),
    ))

if __name__ == '__main__':
    main()
