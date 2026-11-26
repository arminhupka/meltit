# MELTIT FX Plugin

MELTIT w formacie Standalone i VST3 (vendor: Producersources). Presety są wbudowane we wtyczkę; przy pierwszym uruchomieniu, gdy folderu nie ma lub jest pusty, rozpakowują się do `%APPDATA%\Producersources\MELTIT\Presets` (macOS: `~/Library/Application Support/Producersources/MELTIT/Presets`). Zależności JUCE 9.0.2 i Signalsmith są dostępne lokalnie w `ThirdParty/`.

## Budowanie na Windows

Wymagany jest CMake 3.22 lub nowszy oraz Visual Studio 2022 z pakietem Desktop development with C++.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-release
ctest --preset windows-release
```

### Podgląd GUI bez DAW

```powershell
cmake --preset windows-x64
cmake --build build/windows-x64 --config Release --target MELTIT_Standalone
& "./build/windows-x64/MELTIT_artefacts/Release/Standalone/MELTIT.exe"
```


CMake generuje projekt Visual Studio w `build/windows-x64/`. Pakiet VST3 trafia do `build/windows-x64/MELTIT_artefacts/Release/VST3/`.

## Katalogi

- `Source/` zawiera procesor, edytor i obsługę presetów. Moduły DSP są w `Source/Dsp/`.
- `Assets/` zawiera grafiki PNG i fonty osadzane w Standalone i VST3.
- `Tests/` zawiera testy natywne uruchamiane przez CTest i skrypty porównujące render audio.
- `Resources/Presets/` zawiera bibliotekę skonwertowanych presetów.
- `Resources/Design/` zawiera makiety, podglądy, prompty i zebrane grafiki GUI.
- `Resources/Releases/` zawiera archiwalne paczki instalacyjne.
- `Tools/` zawiera importer presetów.
- `Archive/` zawiera wcześniejsze prototypy, pomiary, skrypty wydań i katalogi kompilacji.
