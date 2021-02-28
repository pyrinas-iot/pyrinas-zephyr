interface OTAVersion {
    major: number,
    minor: number,
    patch: number,
    commit: number,
    hash: Array<number>
}

interface OTAManifest {
    version: OTAVersion,
    file: string,
    force: boolean
}

async function main() {

    // Run the Git command
    const p = Deno.run({
        cmd: ["git", "describe", "--tags", "--long"],
        stdout: "piped",
        stderr: "piped",
    });

    // Get the code
    const { code } = await p.status();

    // Make sure output is correct
    if (code === 0) {
        let rawOutput = await p.output();

        // Get the version
        let ver = new TextDecoder().decode(rawOutput).replace(/(\r\n|\n|\r)/gm, "");
        console.log(`version ${ver}`);

        // Set up manifest
        let manifest: OTAManifest = {
            version: {
                major: 0, minor: 0, patch: 0, commit: 0, hash: new Array(8)
            },
            file: "",
            force: false,
        };

        // Then process it!
        manifest.force = false;
        manifest.file = "app_update.bin";

        let match = ver.match("([0-9])+\.([0-9])+\.([0-9])+-([0-9])+-(.{8})");

        if (match != null) {

            // Version numbers
            manifest.version.major = +match[1];
            manifest.version.minor = +match[2];
            manifest.version.patch = +match[3];
            manifest.version.commit = +match[4];
            manifest.version.hash = Array.from(new TextEncoder().encode(match[5]));

        } else {

            console.error("Unable to process manifest. Invalid git describe format.");
            return -1;

        }

        console.log("manifest generated!");
        console.log(manifest);

        // Write to text file
        const write = Deno.writeTextFile("./manifest.json", JSON.stringify(manifest));
        write.then(() => console.log("File written to ./manifest.json"));

    }

}

main();