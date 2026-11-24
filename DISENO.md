# Diseño de nuestra versión de Hearthcooking

Fork de Hearthcooking (evodube, GPL-3.0). Este documento recoge las reglas acordadas para nuestra versión.

## 1. Cocina Rápida

Se mantiene igual: abre el menú de cocina del propio juego y da el plato vanilla (calidad Normal).

## 2. El fuego (estilo Minecraft)

- Cada estación (horno, olla, asador) empieza con un número **aleatorio de 2 a 5 leños** disponibles.
- También se puede usar la **Leña (Firewood) del inventario** del jugador; cada leño usado se descuenta.
- Sin leña en la estación ni en el inventario solo queda la Cocina Rápida.
- Un leño arde **20 segundos** y se apaga; hay que recargar. Una barra por leño muestra cuánto le queda.
- **Mientras haya al menos un leño ardiendo, la estación cocina.** Si no hay ninguno, la cocción se **pausa**.
- Leños ardiendo a la vez acortan el tiempo, cada uno la mitad que el anterior:

| Leños ardiendo | Reducción del tiempo |
| --- | --- |
| 1 | 0 % |
| 2 | 10 % |
| 3 | 15 % |
| 4 | 17,5 % |
| 5 (máximo) | 18,75 % |

## 3. El tiempo y la calidad

- Cada receta tiene su tiempo base (con 1 leño). Valores iniciales:

| Tipo | Tiempo base |
| --- | --- |
| Sweetroll y dulces pequeños | 12 s |
| Asado / carne a la parrilla | 15 s |
| Pan | 17 s |
| Guiso / sopa | 20 s |
| Pastel (pie) | 22 s |

  Ajustables en el `.ini` por receta.
- En la mesa hay un **reloj de arena** y un **contador numérico**. Se quita la barra de progreso del mod original.
- Con `T` = el tiempo de esa pieza, la calidad depende del segundo en que sale:

| Sale en | Calidad |
| --- | --- |
| antes de `T` | **Crudo** (se trata igual que Quemado) |
| `T` a `T+1` | **Obra Maestra** |
| `T+1` a `T+4` | **Buena** |
| `T+4` a `T+7` | **Normal** (plato vanilla) |
| después de `T+7` | **Quemado** |

  Ejemplo con T = 10: sacarlo en 1-9 da Crudo, en el 10 Obra Maestra, en 11-13 Buena, en 14-16 Normal y desde el 17 Quemado.

## 4. Recompensas

| Calidad | Potencia del efecto | Duración | Precio |
| --- | --- | --- | --- |
| Obra Maestra | x3,5 | x2,5 | x3,5 |
| Buena | x2 | x2 | x2 |
| Normal | x1 | x1 | x1 |
| Quemado / Crudo | x0,5 | x0,5 | x0,5 |

## 5. Varios platos a la vez

- Antes de cocinar, un **slider** elige cuántos. El máximo lo ponen los ingredientes: si la receta pide 1 de harina y
  tienes 3, máximo 3.
- Máximos por estación: horno 4, asador 4, olla 5.

### Horno (por tandas) — REDISEÑO ACORDADO (pendiente de programar)
Reemplaza la franja de corte de la v0.6 (se quita).
- Los ingredientes aparecen multiplicados por N en la mesa.
- **Recetas con masa** (pasteles, apple dumplings, pan, sweetrolls...): la harina y demás van al bol y sale **una sola
  masa grande**. Se amasa con una **barra de progreso de amasado** (la masa grande pide ~1,5x el amasado normal).
  Luego se **corta en 3D con el cuchillo en la tabla** (como las verduras) en N trozos; el tamaño de cada trozo
  decide su tiempo en el horno.
- **Recetas sin masa** (garlic bread y similares): se prepara cada pieza N veces, una por una.
- Los trozos esperan en un **compartimento abajo de la mesa**. En el centro se arma uno por uno (rellenar,
  cerrar, doblar...). Al terminar uno se baja a la **bandeja de horneado** y se sube el siguiente, hasta N.
- **La pala ya no lleva la masa sola: mete la bandeja entera al horno y la saca entera.** Luego se emplata, un
  plato por pieza, y termina el proceso.

### Caos (todas las estaciones)
- Se quitan los bloqueos de orden ("echa la sal primero", "pon leña primero", "la masa no está completa"...).
  El jugador puede saltarse pasos o hacerlos en otro orden.
- Castigo: **cada paso saltado o fuera de orden baja un nivel** de calidad (Obra Maestra → Buena...). Errores
  graves (masa sin amasar, pastel sin cerrar) dejan el plato como máximo en Normal.
- **El error se ve en el modelo del plato**: un sweetroll mal hecho sale aplastado o "mushy" (deformado, pálido o
  irregular), no solo con otro nombre.

### Asador
- Los ingredientes aparecen multiplicados por N (pendiente).
- Hasta 4 presas, **cada una con su temporizador**, que empieza cuando se monta. Se sacan una por una.

### Olla
- Los ingredientes aparecen multiplicados por N y se cortan todos (pendiente).
- Todo se cocina junto. Al servir con el cucharón, **cada plato se califica en el segundo en que se sirve**. El
  primero puede ser Obra Maestra y los siguientes van saliendo peor.

## 6. Interfaz

Estilo fantasía elegante (tipo The Witcher 3): paneles oscuros con textura, marcos con ornamentos dorados,
tipografía serif, reloj de arena ilustrado. Todo arte propio; nada copiado de otros juegos.

## Fases

1. Núcleo de reglas: fuego por leños, temporizadores y calidades (`src/sim/Timing.*`, con tests).
2. Integración en el juego: reloj de arena, contador, leña del inventario, quitar la barra. (hecho, falta compilar y probar)
3. Varios platos: slider, corte de masa, tandas del horno, 4 presas en el asador, servir en la olla.
4. Interfaz nueva.

## Estado (24 nov 2026)
Hecho (v0.7, rama `nuestra-version` del fork uwudermus/Hearthcooking; cada push compila solo y publica el mod en la
rama `dist`): reglas de tiempo y calidad, leña de 20 s con barra, porciones con slider, asador con varias carnes,
olla con varios cuencos, platos por porción con su calidad debajo, barras verticales de tiempo, autoparche de
listas de comida (iNeed), debug de 20 ingredientes, interfaz oscura con dorado.
Siguiente: el rediseño del horno de arriba (en dos entregas) y el caos.
