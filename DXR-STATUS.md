# Estado del trabajo de ray tracing (DXR)

Nota de handoff. Describe qué está implementado, qué **no** funciona todavía y qué
queda por hacer. No es documentación de una característica terminada: **el ray tracing
aún no se ve en pantalla.**

## Cómo activarlo

Todo el camino es opt-in y está apagado por defecto. El juego se comporta igual que
sin estos cambios si no defines las variables.

```
GALAXY_DXR_CAPTURE=1     captura de estructuras de aceleración + pass de ray query
GALAXY_DXR_MEASURE=1     contadores por draw (diagnóstico)
GALAXY_DXR_BISECT=n      controles de bisección, ver abajo
```

Con `GALAXY_DXR_CAPTURE=1` el juego corre estable: sin device removal, sin cuelgue de
GPU, y la captura registra geometría real sin rechazar ningún draw.

## Qué funciona

**Build del shader.** DXC está vendorizado en `third_party/dxc` (19.7 MB). CMake compila
`runtime/src/gx/dxr_rayquery.hlsl` a DXIL con `-T cs_6_5` y lo embebe como array C vía
`cmake/embed_binary.cmake`. Sin esto el build no tiene nada que cargar.

**Captura de aceleración structures.** `runtime/src/gx/dxr_capture.{h,cpp}` construye una
BLAS por primitiva directamente desde el vertex ring del frame (posiciones = 3 floats,
stride 144) y usa la matriz de mundo del draw como transform de instancia en la TLAS.
Sin copia de vértices, sin caché persistente, todo reconstruido por frame. Medido antes
en un spike: 580 primitivas ≈ 1.3 ms.

**Validación de entradas.** `add_draw()` rechaza índices fuera del rango real del view del
draw, draws no-indexeados con más índices que vértices, `vertex_count` por encima de 65536
con índices R16, y transform con NaN/Inf.Motivo: una estructura malformada no falla el
build, **cuelga la GPU durante el traversal**. Los contadores `skipped_out_of_range` y
`skipped_non_finite_transform` dicen cuántos draws se descartan (actualmente 0).

**Compute pass RayQuery.** PSO, heaps de descriptores, dispatch y salida a textura UAV,
todo en `DxrCapture::record_trace()`. El shader traza rayos primarios desde la cámara del
juego contra la TLAS del frame. Sombreado aproximado: tinte por instancia y una luz
direccional. **Las texturas y materiales del juego no están bound en este pass**, así que
no es un espejo fiel de la imagen rasterizada.

**Cámara.** `GxBackend::capture_scene_projection()` pasa el bloque de constantes del vertex
shader (que ya es el view-projection completo: `o.pos = mul(projection, wpos)`) al renderer,
que lo invierte por Gauss-Jordan. Sin la cámara el pass no puede construir rayos.

## Qué NO funciona

Tres bloqueos, en orden de dependencia:

1. **`trace-ready=0`.** Con la cámara ya válida (`proj-valid=1`), `initialize_trace()` sigue
   fallando en silencio y el PSO nunca se crea. Hay que registrar su HRESULT para ver por qué.

2. **El rango de rayos está mal.** Log actual: `near=0.5 far=1`. Viene de la fórmula de
   perspectiva de GX, que no es válida con `projtype=1`. Un rango `[0.5, 1]` en unidades de
   mundo **recorta todos los rayos**, así que la imagen saldría vacía aunque el dispatch
   corriera. Hay que derivarlo de la matriz, o usar un rango amplio por defecto.

3. **No hay blend.** Aunque el dispatch produzca una imagen correcta en su textura UAV,
   **nada la lee**. Falta el slice E: componer el resultado en el blit de presentación y
   exponer el toggle `rt_reflections`. Este es el trabajo grande que falta.

## Una corrección importante sobre diagnósticos previos

Varias conclusiones anteriores fueron **erróneas por binarios obsoletos**: el chequeo de
errores de compilación buscaba `" error "` mientras clang emite `error:`, así que los builds
fallaban en silencio y se reinstalaba el binario anterior. Los symptoms `E_INVALIDARG`,
`DXGI_ERROR_DEVICE_HUNG (0x887a0006)` y la conclusión de que `DxrCapture::initialize()` era
el culpable **no eran reales**. Con binarios válidos y error checking correcto no hay
ninguno de esos fallos.

Si retomas esto, verifica siempre que el build terminó antes de copiar el ejecutable.

## Detalles del entorno que importan

El `d3d12.h` que trae el toolchain de Nebula es de un SDK de ~2017 (MIDL 8.01). Falta
`CopyTextureToBuffer` y `CopyBufferToMemory` (no hay readback por GPU posible),
`GetDescriptorTableFromHeap`, y `CreateRootSignature` solo existe en su forma serializada
(hay que pasar por `D3D12SerializeRootSignature`).

`D3D12_RAYTRACING_ACCELERATION_STRUCTURE_SRV` contiene `Location` (una dirección), no un
puntero a recurso.

Una barrera UAV explícita contra un recurso en `D3D12_RESOURCE_STATE_COMMON` es **inválida**
y hace fallar `ID3D12GraphicsCommandList::Close()` con `E_INVALIDARG`. El orden entre el
build de la TLAS y el dispatch se resuelve transicionando la estructura a
`D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE`.

## Controles de bisección

`GALAXY_DXR_BISECT` sirve para localizar en qué subsistema falla, sin casos raros:

- `1` — construye las estructuras, no despacha.
- `2` — despacha contra TLAS vacía (ningún rayo atraviesa geometría real).
- `3` — captura inicializada, cero comandos DXR grabados.
- `4` — captura inicializada, pero nunca se llama a `add_draw`.

Estos se usaron para descartar de forma concluyente el build de AS y el RayQuery como
causa del fallo. Son baratos e inertes; conviene conservarlos hasta que el pass funcione.

## Estado del toggle del juego

`runtime/src/gx/render_config.cpp` sigue forzando `ray_tracing_enabled`, `rt_ao_enabled` y
`rt_reflections` a `false`. No se ha tocado todavía: el pass experimental se controla solo
con `GALAXY_DXR_CAPTURE`.

## DXC no está en el repositorio

El shader RayQuery necesita **DXC**, no fxc: RayQuery no tiene equivalente en DXBC y
D3D12 requiere el contenedor DXIL. El `d3d12.h` del toolchain es de 2017 y su fxc no
puede emitirlo.

`third_party/dxc/` está en `.gitignore` a propósito: son ~20 MB de binario que no cambian
nunca y que ensuciarían el historial para siempre. En su lugar, CMake lo busca primero en
`third_party/dxc` y luego en el `PATH`. Si no lo encuentra, la configuración falla con un
mensaje claro en vez de dejar el header del shader vacío en silencio.

Para compilar el pass hace falta DXC en el PATH. Descarga: <https://github.com/microsoft/DirectXShaderCompiler/releases>
(el paquete `dxc_YYYY_MM_DD.zip`). Verifica con `dxc --version`; necesitas 1.9 o posterior,
porque los opcodes de RayQuery no son válidos antes de `cs_6_3` y este shader usa `cs_6_5`.

El build de todo lo demás no necesita DXC: solo lo necesita el target que genera el shader.


## Siguientes pasos

1. Registrar el HRESULT de `initialize_trace()` y arreglarlo.
2. Derivar `near`/`far` de la matriz, no de los registros de proyección de GX.
3. Slice E: blend de la textura trazada en el blit de presentación + toggle.
4. Con material y texturas bound en el pass, el sombreado deja de ser aproximado.
5. Retirar `GALAXY_DXR_MEASURE` y los contadores cuando el pass sea estable.
