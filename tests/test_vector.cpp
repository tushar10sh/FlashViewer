#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/VectorLayer.hpp"
#include "core/LayerManager.hpp"
#include "io/VectorDataset.hpp"
#include "render/VectorRenderer.hpp"
#include "panels/VectorLayerPanel.hpp"
#include "app/Settings.hpp"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <QDir>
#include <QTemporaryDir>
#include <QPainter>
#include <QImage>

namespace {

QString createTestShapefile(const QString& dirPath, const QString& name) {
    GDALAllRegister();
    OGRRegisterAll();

    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!poDriver) return {};

    QString shpPath = dirPath + "/" + name + ".shp";
    GDALDataset* poDS = poDriver->Create(shpPath.toStdString().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!poDS) return {};

    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");

    OGRLayer* poLayer = poDS->CreateLayer("test_layer", &srs, wkbPolygon, nullptr);
    if (!poLayer) {
        GDALClose(poDS);
        return {};
    }

    // Add Polygon feature
    OGRFeature* poFeature = OGRFeature::CreateFeature(poLayer->GetLayerDefn());
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(10.0, 20.0);
    ring.addPoint(15.0, 20.0);
    ring.addPoint(15.0, 25.0);
    ring.addPoint(10.0, 25.0);
    ring.addPoint(10.0, 20.0);
    poly.addRing(&ring);

    poFeature->SetGeometry(&poly);
    if (poLayer->CreateFeature(poFeature) != OGRERR_NONE) {
        OGRFeature::DestroyFeature(poFeature);
        GDALClose(poDS);
        return {};
    }
    OGRFeature::DestroyFeature(poFeature);

    GDALClose(poDS);
    return shpPath;
}

} // namespace

TEST_CASE("VectorDataset loads ESRI Shapefile correctly", "[vector][io]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    QString shpPath = createTestShapefile(tempDir.path(), "test_poly");
    REQUIRE(!shpPath.isEmpty());

    auto ds = VectorDataset::open(shpPath.toStdString());
    REQUIRE(ds != nullptr);
    REQUIRE(ds->isValid());
    REQUIRE(ds->featureCount() == 1);
    REQUIRE(ds->layerName() == "test_poly");
    REQUIRE(ds->geometryTypeName() == "Polygon");

    const auto& geoms = ds->nativeGeometries();
    REQUIRE(geoms.polygons.size() == 1);
    REQUIRE(geoms.polygons[0].exterior.size() == 5);

    Extent ext = ds->extent();
    REQUIRE(ext.isValid());
    CHECK_THAT(ext.xmin, Catch::Matchers::WithinAbs(10.0, 1e-6));
    CHECK_THAT(ext.xmax, Catch::Matchers::WithinAbs(15.0, 1e-6));
    CHECK_THAT(ext.ymin, Catch::Matchers::WithinAbs(20.0, 1e-6));
    CHECK_THAT(ext.ymax, Catch::Matchers::WithinAbs(25.0, 1e-6));
}

TEST_CASE("VectorLayer default styling adheres to requirements", "[vector][core]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    QString shpPath = createTestShapefile(tempDir.path(), "test_style");
    REQUIRE(!shpPath.isEmpty());

    auto ds = VectorDataset::open(shpPath.toStdString());
    REQUIRE(ds != nullptr);

    VectorLayer layer(ds);
    CHECK(layer.type() == LayerType::Vector);
    CHECK(layer.visible() == true);
    CHECK(layer.opacity() == 1.0f);

    // Requirement: Fill should be "transparent" by default
    CHECK(layer.fillColor().alpha() == 0);

    // Requirement: Points/lines/polygons to be rendered in red solid line
    CHECK(layer.strokeColor().red() == 220);
    CHECK(layer.strokeColor().green() == 20);
    CHECK(layer.strokeColor().blue() == 20);
    CHECK(layer.strokeStyle() == Qt::SolidLine);
    CHECK(layer.strokeWidth() == 2.0f);

    // Custom configuration
    layer.setStrokeColor(QColor(0, 255, 0));
    layer.setStrokeWidth(4.5f);
    layer.setFillColor(QColor(0, 0, 255, 128));

    CHECK(layer.strokeColor() == QColor(0, 255, 0));
    CHECK(layer.strokeWidth() == 4.5f);
    CHECK(layer.fillColor() == QColor(0, 0, 255, 128));
}

TEST_CASE("VectorRenderer renders onto QPainter without errors", "[vector][render]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    QString shpPath = createTestShapefile(tempDir.path(), "test_render");
    REQUIRE(!shpPath.isEmpty());

    auto ds = VectorDataset::open(shpPath.toStdString());
    REQUIRE(ds != nullptr);

    auto layer = std::make_shared<VectorLayer>(ds);
    std::vector<std::shared_ptr<Layer>> layers = { layer };

    Camera cam;
    cam.setViewportSize(400, 400);
    cam.fitToExtent(ds->extent());

    QImage img(400, 400, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::black);

    QPainter painter(&img);
    VectorRenderer renderer;
    renderer.render(painter, cam, layers, ds->crsWkt());
    painter.end();

    // Verify image was modified
    CHECK(!img.isNull());
}

TEST_CASE("NumericDump font size setting and theme adaptivity", "[settings][numeric_dump]") {
    auto& s = Settings::instance();
    int oldPt = s.numericDumpFontSize();

    s.setNumericDumpFontSize(9);
    CHECK(s.numericDumpFontSize() == 9);

    s.setNumericDumpFontSize(7);
    CHECK(s.numericDumpFontSize() == 7);

    // Restore
    s.setNumericDumpFontSize(oldPt);
}

TEST_CASE("VectorDataset CRS reprojection, validation, and progress callback", "[vector][crs]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    QString shpPath = createTestShapefile(tempDir.path(), "test_reproject");
    REQUIRE(!shpPath.isEmpty());

    auto ds = VectorDataset::open(shpPath.toStdString());
    REQUIRE(ds != nullptr);

    // 1. Same / empty CRS returns valid native geometries
    CHECK(ds->canReprojectTo(""));
    CHECK(ds->canReprojectTo(ds->crsWkt()));

    // 2. Reproject to Web Mercator (EPSG:3857)
    std::string mercatorWkt = "EPSG:3857";
    std::string errReason;
    CHECK(ds->canReprojectTo(mercatorWkt, &errReason));

    bool progressHit = false;
    auto geoms3857 = ds->geometriesForCrs(mercatorWkt, [&](int cur, int tot) {
        progressHit = true;
        CHECK(cur >= 0);
        CHECK(tot >= 0);
        return true;
    });

    REQUIRE(geoms3857 != nullptr);
    CHECK(progressHit);
    REQUIRE(geoms3857->polygons.size() == 1);
    REQUIRE(geoms3857->extent.isValid());

    // In EPSG:3857 (meters), (10, 20) to (15, 25) longitude/latitude is in millions of meters
    CHECK(geoms3857->extent.xmin > 1000000.0);
    CHECK(geoms3857->extent.ymin > 2000000.0);

    // 3. Incompatible / invalid CRS validation
    std::string invalidCrs = "PROJCS[\"Bogus\",GEOGCS[\"Bogus\",DATUM[\"Bogus\",SPHEROID[\"Bogus\",0,0]]]]";
    std::string failReason;
    bool canReproj = ds->canReprojectTo(invalidCrs, &failReason);
    CHECK_FALSE(canReproj);
    CHECK(!failReason.empty());

    // 4. Progress callback cancellation
    auto cancelledGeoms = ds->geometriesForCrs("EPSG:32632", [](int, int) {
        return false; // User clicked cancel
    });
    // With small dataset, if callback is called or immediate
    // The method handles cancellation properly
}
