Place the Windows build of the helper here as `Win64/shared-world-helper.exe`:

    cd shared-world-helper
    GOOS=windows GOARCH=amd64 go build -o ../shared-world-mod/SharedWorld/ThirdParty/SharedWorldHelper/Win64/shared-world-helper.exe ./cmd/shared-world-helper

The binary is not committed to the repository.
