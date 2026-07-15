param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $CommandLine
)

$ErrorActionPreference = "Stop"

if ($CommandLine.Count -eq 0) {
    throw "Cargo runner expected an executable path."
}

$opencvBin = "D:\librarys\opencv\build\x64\vc14\bin"
$llvmBin = "D:\LLVM\bin"
$env:PATH = "$opencvBin;$llvmBin;$env:PATH"

$program = $CommandLine[0]
$programArgs = if ($CommandLine.Count -gt 1) {
    $CommandLine[1..($CommandLine.Count - 1)]
} else {
    @()
}

& $program @programArgs
exit $LASTEXITCODE
