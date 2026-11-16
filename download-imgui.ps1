
$ErrorActionPreference = "Stop"

$base = "https://raw.githubusercontent.com/ocornut/imgui/master"

$files = @(
    "imgui.h",
    "imgui_internal.h",
    "imconfig.h",
    "imstb_textedit.h",
    "imstb_rectpack.h",
    "imstb_truetype.h",
    "imgui.cpp",
    "imgui_draw.cpp",
    "imgui_tables.cpp",
    "imgui_widgets.cpp",
    "backends/imgui_impl_win32.h",
    "backends/imgui_impl_win32.cpp",
    "backends/imgui_impl_dx11.h",
    "backends/imgui_impl_dx11.cpp"
)

foreach ($file in $files) {
    $destination = Join-Path "src" ($file -replace "/", "\")

    $directory = Split-Path $destination

    if (!(Test-Path $directory)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }

    if (!(Test-Path $destination)) {
        Write-Host "Downloading $file"
        Invoke-WebRequest "$base/$file" -OutFile $destination
    }
}

Write-Host "All ImGui dependencies are ready."
