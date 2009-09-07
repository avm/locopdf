#ifndef POINT_H
#define POINT_H

template <typename T> struct Point {
	T x;
	T y;

	Point(T ix, T iy): x(ix), y(iy) {}
	Point operator+ (const Point& p) const {return Point(x+p.x, y+p.y);}
	Point operator- (const Point& p) const {return Point(x-p.x, y-p.y);}
	Point operator/ (const Point& p) const {return Point(x/p.x, y/p.y);}
	Point operator/ (T s) const {return Point(x/s, y/s);}
	Point operator* (const Point& p) const {return Point(x*p.x, y*p.y);}
	Point operator* (T s) const {return Point(x*s, y*s);}
};

typedef Point<double> PointD;

#endif /* ifndef POINT_H */
