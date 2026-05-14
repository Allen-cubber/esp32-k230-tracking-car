$ErrorActionPreference = "Stop"

$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $ProjectRoot "manual_build"
$SdkDir = $env:ANDROID_HOME
if (-not $SdkDir) {
    $SdkDir = $env:ANDROID_SDK_ROOT
}
if (-not $SdkDir) {
    $SdkDir = "C:\Users\24696\AppData\Local\Android\Sdk"
}

$BuildTools = Join-Path $SdkDir "build-tools\37.0.0"
$PlatformJar = Join-Path $SdkDir "platforms\android-36.1\android.jar"
$JbrBin = "C:\Program Files\Android\Android Studio\jbr\bin"

function Run-Step($Command, [string[]]$ArgsList) {
    & $Command @ArgsList
    if ($LASTEXITCODE -ne 0) {
        throw "failed: $Command $($ArgsList -join ' ')"
    }
}

$env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"
$env:PATH = "$env:JAVA_HOME\bin;$env:PATH"

Remove-Item -Recurse -Force $BuildDir -ErrorAction SilentlyContinue
foreach ($Dir in @("compiled", "gen", "classes", "dex", "out")) {
    New-Item -ItemType Directory -Force (Join-Path $BuildDir $Dir) | Out-Null
}

Run-Step (Join-Path $BuildTools "aapt2.exe") @(
    "compile", "--dir", (Join-Path $ProjectRoot "app\src\main\res"),
    "-o", (Join-Path $BuildDir "compiled\res.zip")
)

Run-Step (Join-Path $BuildTools "aapt2.exe") @(
    "link",
    "-o", (Join-Path $BuildDir "out\app-unsigned.apk"),
    "-I", $PlatformJar,
    "--manifest", (Join-Path $ProjectRoot "app\src\main\AndroidManifest.xml"),
    "--java", (Join-Path $BuildDir "gen"),
    "--min-sdk-version", "23",
    "--target-sdk-version", "36",
    (Join-Path $BuildDir "compiled\res.zip")
)

Run-Step (Join-Path $JbrBin "javac.exe") @(
    "-source", "8",
    "-target", "8",
    "-bootclasspath", $PlatformJar,
    "-classpath", $PlatformJar,
    "-encoding", "UTF-8",
    "-d", (Join-Path $BuildDir "classes"),
    (Join-Path $ProjectRoot "app\src\main\java\com\trackingcar\dashboard\MainActivity.java"),
    (Join-Path $BuildDir "gen\com\trackingcar\dashboard\R.java")
)

Run-Step (Join-Path $JbrBin "jar.exe") @(
    "cf", (Join-Path $BuildDir "classes.jar"),
    "-C", (Join-Path $BuildDir "classes"),
    "."
)

Run-Step (Join-Path $BuildTools "d8.bat") @(
    "--min-api", "23",
    "--lib", $PlatformJar,
    "--output", (Join-Path $BuildDir "dex"),
    (Join-Path $BuildDir "classes.jar")
)

Copy-Item (Join-Path $BuildDir "out\app-unsigned.apk") (Join-Path $BuildDir "out\app-with-dex.apk")
Run-Step (Join-Path $JbrBin "jar.exe") @(
    "uf", (Join-Path $BuildDir "out\app-with-dex.apk"),
    "-C", (Join-Path $BuildDir "dex"),
    "."
)

Run-Step (Join-Path $BuildTools "zipalign.exe") @(
    "-f", "4",
    (Join-Path $BuildDir "out\app-with-dex.apk"),
    (Join-Path $BuildDir "out\tracking-car-dashboard-unsigned.apk")
)

$KeyStore = Join-Path $BuildDir "debug.keystore"
Run-Step (Join-Path $JbrBin "keytool.exe") @(
    "-genkeypair", "-v",
    "-keystore", $KeyStore,
    "-storepass", "android",
    "-keypass", "android",
    "-alias", "androiddebugkey",
    "-keyalg", "RSA",
    "-keysize", "2048",
    "-validity", "10000",
    "-dname", "CN=Android Debug,O=Android,C=US"
)

$Apk = Join-Path $ProjectRoot "tracking-car-dashboard-debug.apk"
Run-Step (Join-Path $BuildTools "apksigner.bat") @(
    "sign",
    "--ks", $KeyStore,
    "--ks-pass", "pass:android",
    "--key-pass", "pass:android",
    "--out", $Apk,
    (Join-Path $BuildDir "out\tracking-car-dashboard-unsigned.apk")
)

Run-Step (Join-Path $BuildTools "apksigner.bat") @("verify", "--verbose", $Apk)
Get-Item $Apk
