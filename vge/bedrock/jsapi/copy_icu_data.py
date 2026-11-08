import os

import buildconfig


def main(output):
    source = os.path.join(buildconfig.substs["VGE_V8_DIR"], "Data", "icudtl.dat")
    with open(source, "rb") as data:
        output.write(data.read())
    return {source}
