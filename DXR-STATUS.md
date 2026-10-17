# Estado del trabajo de ray tracing (DXR)

Nota de cierre. El proyecto se deja pausado: **el ray tracing nunca se vio en
pantalla**, y el camino se cerro por restricciones del entorno, no por el
diseno de la solucion.

Todo el trabajo esta commiteado y el camino esta diagnosticado. Ver
"Estado final" mas abajo para el resumen y para que se deberia retomar.

## Como activarlo

Todo el camino es opt-in y esta apagado por defecto. Sin estas variables el
juego se comporta igual que sin estos cambios.

```
GALAXY_DXR_CAPTURE=1     captura de estructuras + pass de ray query
GALAXY_DXR_PROBE=1       shader equivalente SIN ray query (ver pruebas)
GALAXY_DXR_BLEND=<f>     intensidad del blend, 0 lo desactiva
GALAXY_DXR_MEASURE=1     contadores por draw (diagnostico)
GALAXY_DXR_BISECT=n      controles de bisection
GALAXY_DXR_BUILD_STAGE=n    1 = solo BLAS, 2 = + copia de instancias
GALAXY_DXR_TRACE_STAGE=n    1 = no graba el pass, 3 = completo
GALAXY_DXR_FORCE_TRACE=1     crea el PSO aunque no haya estructuras
```

## Que funciona

- **Build del shader.** DXC se busca en `third_party/dxc` o en el PATH, y CMake
  compila `dxr_rayquery.hlsl` a DXIL (`cs_6_5`) y lo embebe con
  `cmake/embed_binary.cmake`. RayQuery no tiene equivalente en DXBC, asi que
  fxc no sirve.

- **Captura de acceleration structures.** `dxr_capture.{h,cpp}` construye una
  BLAS por primitiva desde el vertex ring del frame (posiciones = 3 floats,
  stride 144) y usa la matriz de mundo del draw como instancia de la TLAS. Sin
  copia de vertices, sin cache persistente.

- **Validacion de entradas.** `add_draw()` rechaza indices fuera del rango real
  del view, draws con mas indices que vertices, `vertex_count` sobre 65536 con
  indices R16, y transform con NaN/Inf. Una estructura malformada no falla el
  build: cuelga la GPU durante el traversal.

- **Compute pass RayQuery.** PSO, heaps, dispatch y salida a textura UAV en
  `record_trace()`. Sombreado aproximado: tinte por instancia y una luz
  direccional. Las texturas y materiales del juego NO estan bound, asi que no
  es un espejo fiel de la imagen rasterizada.

- **Camara.** `capture_scene_projection()` pasa el bloque de constantes del
  vertex shader (que ya es el view-projection completo) y el renderer lo
  invierte por Gauss-Jordan. El rango de rayos sale de des-proyectar NDC
  z=0 y z=1, no de los registros de proyeccion de GX (que aqui dan 0.5/1 y
  recortan todos los rayos).

- **Blend.** `record_dxr()` compone la textura trazada sobre el frame. Compilado
  por fxc a `ps_5_0`, o sea **no depende del driver ni de RayQuery**, y se puede
  construir y depurar en hardware que no compila ray query.

## Bugs corregidos

Cinco, todos reales. Ninguno producia salida visible, sino recursos invalidos:

1. `heap_properties` se indexaba por `D3D12_HEAP_TYPE`, cuyos valores arrancan en
   1, no en 0. `heap_properties[D3D12_HEAP_TYPE_DEFAULT]` leia la entrada de
   UPLOAD. Todos los recursos se creaban en el heap equivocado.
2. Los builds de AS solo pueden leer entradas en `COMMON`, y un recurso en heap
   UPLOAD esta permanentemente en `GENERIC_READ`. Los descriptores de instancia
   y los indices secuenciales estaban en heaps UPLOAD.
3. La TLAS, su scratch y los descriptores se recreaban cada frame, liberando
   memoria que el traversal del frame anterior pudo leer.
4. `SampleDesc.Quality` debe ser 1 y estaba en 0, lo que hacia invalida la
   descripcion de la textura.
5. Faltaba el rango UAV en la firma raiz para `u0`.

## Estado final: proyecto pausado

Dos problemas quedan abiertos.

### 1. El driver no compila inline ray tracing

La 26.8.1 revienta su propio compilador de shaders con cualquier shader de
inline ray tracing, incluido uno de seis lineas. Confirmado de forma aislada e
independiente del punto 2: con `GALAXY_DXR_PROBE=1` (shader equivalente sin ray
query) el proceso sobrevive; con el shader de ray query muere a los dos
segundos en `amdxc64.dll`.

La unica via sin cambiar drivers seria ray tracing por *pipelines* (RTPSO), que
usa un compilador distinto en el driver. Se evaluo y no es viable aca:

- El SDK de Windows 19041 **no declara** `D3D12CreateStateObject` ni la API de
  pipeline. Comprobado tambien en DirectX-Headers, MinGW y el paquete NuGet mas
  reciente: ninguno la trae.
- `d3d12.dll` **no exporta** `D3D12CreateStateObject` (verificado con
  `GetProcAddress`). Si exporta `D3D12GetInterface`, que es la puerta correcta,
  pero recorrerla obliga a declarar a mano varias interfaces COM con sus vtables
  exactas: dias de trabajo, comportamiento indefinido si un metodo queda
  descolocado, y sin garantia de que el driver compile el pipeline.

### 2. El build de la TLAS falla con E_INVALIDARG

`ID3D12GraphicsCommandList::Close()` devuelve `0x80070057`. Aislado a ese unico
comando con `GALAXY_DXR_BUILD_STAGE`: los builds de nivel inferior funcionan. Los
descriptores de instancia se verificaron validos (alineados a 256 bytes, mascaras
correctas, transformadas identidad). Se descartaron como causa el orden de las
barreras, los estados de recursos, la alineacion y la recreacion por frame.

## Que haria distinto

Casi todos los bloqueos fueron del entorno, y varios los fabrique yo.

- **El peor fue mio.** El chequeo de errores de compilacion buscaba
  `" error "` mientras el compilador emite `error:`. No detectaba fallos,
  reinstalaba el binario viejo y yo diagnosticaba sobre datos de una version
  anterior. Costo horas de conclusiones falsas que parecian razonamiento solido.

- Lo que si funciono, siempre, fue **mirar el estado real en el punto del
  fallo**. `heap: type=2` resolvio en un paso lo que tres hipotesis no
  pudieron. Los descriptores de instancia descartaron de golpe la causa mas
  probable. El bisect por etapas localizo el fallo en un comando.

- **Probar el flag antes de medir.** Una corrida sin `GALAXY_DXR_CAPTURE` dio un
  "sobrevive" falso que invalido toda una tanda de resultados.

- **Medir antes de prometer.** En varios momentos anuncie antes de verificar,
  y luego los datos no dieron lo que esperaba.

## Si se retoma

1. Probar el rollback del driver. Es lo unico que ataca la causa directa del
   punto 1, y es barato.
2. Solo con hardware capaz de compilar ray tracing, seguir con el punto 2. Con
   datos validos y un build que funciona, el `E_INVALIDARG` probablemente sea del
   SDK de 2017, igual que `D3D12CreateStateObject`.

Con un entorno que funcione, el codigo existente deberia servir.
