# Decisions

## 2026-07-18 — Convert a central oriented mesh section into the 2D solid mask

Decision:
- Intersect the imported triangle mesh with an oriented plane through the mesh bounding-box centre.
- Reconstruct closed section loops and retain the loop with the largest absolute area.
- Normalize the selected loop to the former verification-circle diameter and centre it in the CFD domain.
- Rasterize the contour boundary before filling its interior.

Reason:
- The Solver already consumes one binary, cell-centred `solid` mask.
- A central plane section converts both STL and OBJ triangle meshes into that existing interface without coupling the Solver to file formats.
- Keeping the largest loop gives deterministic behavior for the current single-obstacle model.

## Continuous geometry

Let the mesh bounding-box centre be

\[
\mathbf{c}
=
\frac{\mathbf{x}_{min}+\mathbf{x}_{max}}{2}.
\]

Let

\[
\theta_x=\frac{\pi}{180}\,\texttt{sliceAngleX},
\qquad
\theta_z=\frac{\pi}{180}\,\texttt{sliceAngleZ}.
\]

The orthonormal section basis is

\[
\mathbf{e}_1=
\begin{bmatrix}
\cos\theta_z\\
\sin\theta_z\\
0
\end{bmatrix},
\quad
\mathbf{e}_2=
\begin{bmatrix}
-\sin\theta_z\cos\theta_x\\
\cos\theta_z\cos\theta_x\\
\sin\theta_x
\end{bmatrix},
\]

\[
\mathbf{n}=
\begin{bmatrix}
\sin\theta_z\sin\theta_x\\
-\cos\theta_z\sin\theta_x\\
\cos\theta_x
\end{bmatrix}.
\]

The section plane is

\[
(\mathbf{x}-\mathbf{c})\cdot\mathbf{n}=0.
\]

For triangle-edge endpoints \(\mathbf{a}\) and \(\mathbf{b}\), define signed
distances

\[
d_a=(\mathbf{a}-\mathbf{c})\cdot\mathbf{n},
\qquad
d_b=(\mathbf{b}-\mathbf{c})\cdot\mathbf{n}.
\]

When \(d_a d_b<0\), the edge-plane intersection is

\[
t=\frac{d_a}{d_a-d_b},
\qquad
\mathbf{x}_{section}=\mathbf{a}+t(\mathbf{b}-\mathbf{a}).
\]

The two-dimensional section coordinates are

\[
q_x=(\mathbf{x}_{section}-\mathbf{c})\cdot\mathbf{e}_1,
\qquad
q_y=(\mathbf{x}_{section}-\mathbf{c})\cdot\mathbf{e}_2.
\]

## In-plane transformation and normalization

Mirroring changes \(q_x\) to \(-q_x\). With

\[
\phi=\frac{\pi}{180}\,\texttt{sliceRotation},
\]

the in-plane rotation is

\[
\begin{bmatrix}
q'_x\\
q'_y
\end{bmatrix}
=
\begin{bmatrix}
\cos\phi & -\sin\phi\\
\sin\phi & \cos\phi
\end{bmatrix}
\begin{bmatrix}
q_x\\
q_y
\end{bmatrix}.
\]

For contour span

\[
s_{section}
=
\max(q'_{x,max}-q'_{x,min},q'_{y,max}-q'_{y,min}),
\]

the scale is

\[
\alpha=
\frac{0.2\min(L_x,L_y)}{s_{section}}.
\]

The domain coordinates are

\[
x=\frac{L_x}{2}+\alpha(q'_x-q'_{x,centre}),
\qquad
y=\frac{L_y}{2}+\alpha(q'_y-q'_{y,centre}).
\]

## Discrete mask

Cell-centre coordinates are

\[
x_i=\left(i+\frac12\right)\Delta x,
\qquad
y_j=\left(j+\frac12\right)\Delta y.
\]

A cell is a boundary cell when its minimum distance to a contour segment is
at most half the cell diagonal:

\[
d_{segment}(x_i,y_j)
\le
\frac12\sqrt{\Delta x^2+\Delta y^2}.
\]

All remaining cells use the even–odd ray-crossing rule:

\[
\texttt{solid}_{i,j}
=
\begin{cases}
1, & \text{odd number of contour crossings},\\
0, & \text{even number of contour crossings}.
\end{cases}
\]

Assumptions:
- Input geometry is a triangle mesh.
- The desired body is represented by the largest closed central section.
- Geometry coordinates may use arbitrary units because the section is normalized.
- The CFD domain dimensions and grid counts are positive.

Failure behavior:
- Missing, unsupported, empty, open, degenerate, or unreconstructable sections use `initCircle`.
