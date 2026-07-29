# Reglas de Proyecto - RMG-K

## ESTILO NATIVO RMG
Cuando el usuario pida aplicar el **"ESTILO NATIVO RMG"** a cualquier ventana, diálogo o componente visual de la aplicación, se deben seguir estrictamente las siguientes pautas de diseño:

1. **Paleta de Colores y Tema**:
   - Usar la paleta nativa de Qt/Fusion Dark del sistema sin agregar contenedores o tarjetas oscuras flotantes con esquinas excesivamente redondeadas.
   - Usar el marco y barra de título nativa del sistema operativo (`QDialog` / `QMainWindow`).

2. **Diseño y Estructura (Layout)**:
   - Margen exterior estándar: `16px`.
   - Espaciado entre elementos: `10px` a `12px`.
   - Organizar los formularios usando `QFormLayout` limpio con alineación a la izquierda.

3. **Campos de Entrada (`QLineEdit`, `QComboBox`, `QCheckBox`)**:
   - Estilo limpio y compacto: `padding: 4px 6px;`.
   - Sin bordes personalizados ni radios de curva mayores a `3px` o `4px`.

4. **Botón Principal (`QPushButton#PrimaryBtn` / `QPushButton#CreateBtn`)**:
   - Fondo Azul RMG: `#0078D7` (`hover`: `#1084e3`, `disabled`: `#505050` con texto `#888888`).
   - Texto blanco en negrita (`font-weight: bold;`).
   - Bordes: `border: none; border-radius: 3px;`.
   - Relleno: `padding: 6px 18px; min-height: 24px;`.

5. **Botones Secundarios (`QPushButton#SecondaryBtn` / `QPushButton#CancelBtn`)**:
   - Relleno estándar: `padding: 6px 16px; min-height: 24px;`.
   - Estilo nativo acorde a la paleta del sistema.
