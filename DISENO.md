# Diseño de nuestra versión de Hearthcooking

Fork de Hearthcooking (evodube, GPL-3.0). Este documento recoge las reglas acordadas para nuestra versión.

## 1. Cocina Rápida

Se mantiene igual: abre el menú de cocina del propio juego y da el plato vanilla (calidad Normal).

## 2. El fuego (estilo Minecraft)

- Cada estación (horno, olla, asador) empieza con un número **aleatorio de 2 a 5 leños** disponibles.
- También se puede usar la **Leña (Firewood) del inventario** del jugador; cada leño usado se descuenta.
- Sin leña en la estación ni en el inventario solo queda la Cocina Rápida.
- Un leño arde **20 segundos** y se apaga; hay que recargar.
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

### Horno (por tandas)
- Se amasa una vez y se **corta la masa** en tantas piezas como platos.
- El tiempo de cada pieza depende de su tamaño respecto a la pieza ideal (masa total / número de platos):
  `T_pieza = T × (tamaño / ideal)`, entre 0,3×T y 2×T. Un corte diminuto puede bajar 10 s a 3 s y quemarse.
- **La tanda sale entera.** Si dos piezas tardan 10 s y una 5 s, eliges entre sacar dos crudas o una quemada.

### Asador
- Hasta 4 presas, **cada una con su temporizador**, que empieza cuando se monta. Se sacan una por una.

### Olla
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
